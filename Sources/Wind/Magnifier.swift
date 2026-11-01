import AppKit
import CoreGraphics
import WindCore

// The magnifier: one event tap for every input, one display-link tick that turns the held binds,
// the pointer, the caret and the lock state into a view, and writes it to the compositor zoom.
// Everything runs on the main thread (the tap and the display link are both main-run-loop sources).

final class Magnifier {
    private(set) var config = Config()
    private var router = InputRouter()
    private var zoom = ZoomController(maxLevel: 30)
    private var lock = LockDetector()
    private var owner = ViewOwnerState()
    let caret = CaretTracker()
    private let raw = RawMouse()
    private var askedRawAccess = false
    private let crosshair = Crosshair()
    let windCursor = WindCursor()

    private var tap: CFMachPort?
    private var link: CADisplayLink?
    private var lastTick: CFTimeInterval = 0

    // Input state, written by the tap.
    private var mods: Mods = []
    private var lastButtonAt: CFTimeInterval = -1e9
    private var lastKeyAt: CFTimeInterval = -1e9
    private var keyAfterButton = false
    private var mouseDx = 0.0, mouseDy = 0.0        // movement since the last tick (for the owner)
    private var lastMoveAt: CFTimeInterval = 0
    private var pendingLockDx = 0.0, pendingLockDy = 0.0
    private var lastLoc: CGPoint?
    private var lastPtr: Pt?                    // pointer after the last tick's writes (speed compensation)
    private var afterWrites: Pt?                // pointer after the last tick's writes, in every mode
    private var handSpeedAvg = 0.0              // hand speed in pt/s at 1x, averaged over ~80 ms
    private var pointerHiddenForCaret = false
    private var handTarget: Pt?                 // pan glide: where the hand has taken the pointer
    private var lastRawAt = 0.0                 // when raw mouse counts last arrived
    private(set) var crosshairAt: CGPoint?      // where Inspect's crosshair is drawn
    private(set) var windCursorAt: CGPoint?     // where Wind's own cursor is drawn (cursor = "wind")
    private var windCursorOffset: Pt?           // its last offset from the shown centre (pointer - exact centre)
    private var prevRawMotion = 0.0
    private var placedThisTick = Pt(0, 0)       // whole points Wind's carries moved the pointer this tick
    private var placedLastTick = Pt(0, 0)
    private var carryView: Pt?                  // the centre this tick's carry ended the view on
    private var pendingJump: Pt?                // a far pointer read waiting for a second one
    private var rawStep: Pt?                    // raw counts this tick (nil when none flow)
    private var prevRawStep: Pt?
    private var pushX = 0.0, pushY = 0.0       // rejected non-hand pointer pushes awaiting their return
    private var redoX = 0.0, redoY = 0.0, redoAge = 0   // an undone carry macOS may re-apply
    private var ptsPerCount = 1.5                 // screen pt the pointer moves per raw count (learnt)
    private var rawMismatch = 0.0, rawDistrusted = false   // how often counts fall short (EMA)
    private var glideUntil = 0.0, glideEnd = Pt(0, 0), glideMode = false, glideFrame = false   // slow-pan glide
    private var lastGlideRead = 0.0, rawSpeedAvg = 0.0, glideCooldown = 0.0
    private var drawHeldTicks = 0               // draw engine: ticks with counts and a still pointer
    private var drawFreeTicks = 0               // and ticks in a row with counts and a moving pointer
    private var tSmooth = Pt(0, 0), tVel = (x: 0.0, y: 0.0)   // smoothed hand target while zooming
    private var userHidCursor = false           // the hide-cursor bind is on
    private var ptrVel = (x: 0.0, y: 0.0)
    private var lastShift = Pt(0, 0)            // how far the last tick moved the view
    private var prevShift = Pt(0, 0)            // and the tick before
    private var tickStartCenter = Pt(0, 0)
    private var speedTravel = 0.0, speedDelta = 0.0, speedSince: CFTimeInterval = 0

    // View state.
    private var screen = Sz(1920, 1080)
    private var center = Pt(960, 540)
    private var virtualPtr = Pt(960, 540)       // the pointer as the view sees it (deltas while locked)
    private var springVel = (x: 0.0, y: 0.0)
    private var zoomedSession = false
    private var lastWritten: (c: Pt, level: Double)?
    private var liveLevel = 1.0                 // the scale the compositor is showing (last write)
    private var liveCenter = Pt(960, 540)       // the centre last written (the compositor floors it)
    private var wroteThisTick = false
    private var idleTicks = 0
    private var scrollGestureZooms = false      // the current trackpad gesture (and momentum) zooms
    private var storedQuickLevel = 0.0
    private(set) var inspect = false
    private var inspectAnchor = CGPoint.zero
    private var inspectRef: Pt?                 // where the frozen pointer really sits
    private var inspectLook = Pt(0, 0)          // the crosshair: where Inspect is looking
    private var pendingPlace: Pt?               // next tick: put the pointer here (leaving Inspect)
    private var returning = false               // the view is gliding back to the pointer
    private var euroX = OneEuro(minCutoff: 1, beta: 0.02), euroY = OneEuro(minCutoff: 1, beta: 0.02)   // render pan smoothing
    /// Render pan smoothing: the pointer as drawn and as apps get it (nil when not smoothing).
    nonisolated(unsafe) private var smoothPointer: Pt?
    private func resetSmooth() { euroX.reset(); euroY.reset(); smoothPointer = nil }
    private var returnOffset: Pt?               // while returning: view minus the pointer's view, gliding to zero
    private var pointerToView = false           // next tick: bring the pointer to the view centre
    private var wasLocked = false
    private var paused = false                  // the user switched Wind off from the menu
    private var simHeld = Set<Action>()         // holds from the command channel (tests)
    var testMods: Mods?                         // modifiers forced by the command channel (tests)

    var onLevelChange: ((Double) -> Void)?
    var onQuitRequested: (() -> Void)?
    private var lastReportedLevel = 1.0

    var level: Double { zoom.level }
    var tapActive: Bool { tap.map { CGEvent.tapIsEnabled(tap: $0) } ?? false }
    var isLocked: Bool { lock.locked }
    var viewOwner: ViewOwner { owner.owner }
    var currentCenter: Pt { center }
    var isPaused: Bool { paused }
    var inspectLookPoint: Pt { inspectLook }

    // MARK: setup

    func apply(_ c: Config) {
        config = c
        let draw = c.engine == "draw" && !DrawZoom.failed && !DrawZoom.handedOver
        DrawZoom.shared.onFailure = { [weak self] in self?.lastWritten = nil; self?.wake() }   // re-zoom with macOS's zoom
        DrawZoom.shared.onHandover = { [weak self] _ in
            // The other engine draws from the next tick: write the view again, and start the
            // pointer model afresh (the two engines move the pointer differently).
            // The other engine shows the same view at once. Starting it from "1x" made the next level
            // write treat the view as pinned to an edge: it jumped to the top-left corner and macOS
            // took the pointer to (0, 0) (traced 2026-10-31).
            guard let self else { return }
            let lv = self.zoom.level
            if self.zoomedSession && lv > 1.0001 {
                self.send(self.center, level: lv, keepPointer: true)
                self.lastWritten = (self.center, lv)
            } else { self.lastWritten = nil; self.liveLevel = 1 }
            self.handTarget = nil; self.lastPtr = nil; self.wake()
        }
        if c.engine != "draw" { DrawZoom.handedOver = false }
        if draw != DrawZoom.enabled {
            if zoomedSession { SkyLight.reset() }      // leave the old engine at 1x first
            DrawZoom.enabled = draw
            lastWritten = nil
            Log.info("engine: \(draw ? "draw (capture and draw, experimental)" : "zoom (macOS)")")
        }
        Log.level = c.logLevel
        let (b, problems) = c.binds()
        router.setBinds(b)
        for p in problems { Log.error("config: \(p)") }
        zoom.setMaxLevel(c.maxLevel)
        zoom.setProfile(inSpeed: c.zoomInSpeed, outSpeed: c.zoomOutSpeed, smooth: c.smoothZoom,
                        accel: c.smoothZoomAccel, rampSeconds: c.smoothZoomRamp)
        zoom.setEaseOut(seconds: c.zoomEaseOutMs / 1000)
        caret.enabled = c.trackCaret || c.trackFocus
        ColorFilter.apply(warmthPct: c.warmthPct, dimPct: c.dimPct)
        lastWritten = nil
        wake()
    }

    func start() -> Bool {
        updateScreen()
        caret.onChange = { [weak self] in
            if let self, self.zoom.level > 1.0001 { self.wake() }
        }
        if SkyLight.isZoomed { SkyLight.reset() }
        guard installTap() else { return false }
        // Wind's own cursor moves to match each view write just before it (carries and level writes
        // come before the frame's last write; a cursor moved only once per frame showed a frame of
        // the new view with the old cursor place, measured on the zoomed screen).
        SkyLight.beforeWrite = { [weak self] c, s in self?.cursorBeforeWrite(c, scale: s) }
        if let s = NSScreen.screens.first {
            let l = s.displayLink(target: self, selector: #selector(tick(_:)))
            l.add(to: .main, forMode: .common)
            l.isPaused = true
            link = l
        }
        NotificationCenter.default.addObserver(forName: NSApplication.didChangeScreenParametersNotification,
                                               object: nil, queue: .main) { [weak self] _ in self?.updateScreen() }
        return true
    }

    func stop() {
        link?.invalidate(); link = nil
        if let t = tap { CGEvent.tapEnable(tap: t, enable: false) }
        endInspect()
        windCursor.show(at: nil, on: false)
        if pointerHiddenForCaret || userHidCursor { pointerHiddenForCaret = false; userHidCursor = false; SkyLight.setCursorHidden(false) }
        caret.stop()
        SkyLight.reset()
        ColorFilter.restore()
    }

    func setPaused(_ p: Bool) {
        paused = p
        if p {
            _ = router.releaseAll()
            zoom.reset()
            endInspect()
            windCursor.show(at: nil, on: false)
            SkyLight.reset(); lastWritten = nil; liveLevel = 1; zoomedSession = false; caret.zoomed = false
            clearSessionFlags()
            reportLevel()
        }
    }

    private func updateScreen() {
        let b = CGDisplayBounds(CGMainDisplayID())
        screen = Sz(b.width, b.height)
        lastWritten = nil                       // a display change may have reset the zoom: re-assert it
        Log.info("display \(Int(b.width))x\(Int(b.height)) pt")
    }

    private func installTap() -> Bool {
        var mask: CGEventMask = 0
        for t: CGEventType in [.keyDown, .keyUp, .flagsChanged, .leftMouseDown, .leftMouseUp, .rightMouseDown,
                               .rightMouseUp, .otherMouseDown, .otherMouseUp, .mouseMoved, .leftMouseDragged,
                               .rightMouseDragged, .otherMouseDragged, .scrollWheel] {
            mask |= CGEventMask(1) << t.rawValue
        }
        let ref = Unmanaged.passUnretained(self).toOpaque()
        let cb: CGEventTapCallBack = { _, type, event, refcon in
            let me = Unmanaged<Magnifier>.fromOpaque(refcon!).takeUnretainedValue()
            return me.handle(type, event)
        }
        guard let t = CGEvent.tapCreate(tap: .cghidEventTap, place: .headInsertEventTap, options: .defaultTap,
                                        eventsOfInterest: mask, callback: cb, userInfo: ref) else {
            Log.error("event tap could not be created (Accessibility permission missing?)")
            return false
        }
        tap = t
        let src = CFMachPortCreateRunLoopSource(nil, t, 0)
        CFRunLoopAddSource(CFRunLoopGetMain(), src, .commonModes)
        CGEvent.tapEnable(tap: t, enable: true)
        Log.info("event tap installed")
        return true
    }

    // MARK: input

    private func handle(_ type: CGEventType, _ e: CGEvent) -> Unmanaged<CGEvent>? {
        let pass = Unmanaged.passUnretained(e)
        if type == .tapDisabledByTimeout || type == .tapDisabledByUserInput {
            Log.error("event tap disabled (\(type.rawValue)), re-enabling")
            for a in router.stopAll() { edge(.up(a)) }
            if let t = tap { CGEvent.tapEnable(tap: t, enable: true) }
            return pass
        }
        if paused { return pass }
        let now = CACurrentMediaTime()
        // Render pan smoothing: a click lands where the (smoothed) cursor is drawn. macOS is not zoomed on
        // render, so the location is plain desktop points. Only presses and releases: at this tap a moved
        // location also moves the pointer, and rewriting every move would keep pulling it back.
        if let sp = smoothPointer, DrawZoom.enabled, [.leftMouseDown, .leftMouseUp, .rightMouseDown, .rightMouseUp,
            .otherMouseDown, .otherMouseUp].contains(type) {
            e.location = CGPoint(x: sp.x, y: sp.y)
        }
        mods = Mods(rawValue: e.flags.rawValue).intersection(.all)

        switch type {
        case .keyDown, .keyUp:
            let code = UInt16(e.getIntegerValueField(.keyboardEventKeycode))
            lastKeyAt = now
            if type == .keyDown {
                if !mods.contains(.cmd) && !mods.contains(.ctrl) { keyAfterButton = true }
                let (sw, ed) = router.triggerDown(.key(code), mods: mods,
                                                  isRepeat: e.getIntegerValueField(.keyboardEventAutorepeat) != 0)
                if let ed { edge(ed) }
                if !sw && zoom.level > 1.0001 && (config.trackCaret || config.trackFocus) { caret.pollSoon() }
                return sw ? nil : pass
            } else {
                let (sw, ed) = router.triggerUp(.key(code))
                if let ed { edge(ed) }
                return sw ? nil : pass
            }

        case .flagsChanged:
            lastKeyAt = now
            return pass

        case .leftMouseDown, .rightMouseDown:
            lastButtonAt = now; keyAfterButton = false
            wake()
            return pass
        case .leftMouseUp, .rightMouseUp:
            lastButtonAt = now
            return pass

        case .otherMouseDown:
            let n = Int(e.getIntegerValueField(.mouseEventButtonNumber))
            let (sw, ed) = router.triggerDown(.mouse(n), mods: mods)
            if let ed { edge(ed); return nil }
            if sw { return nil }
            lastButtonAt = now; keyAfterButton = false
            return pass
        case .otherMouseUp:
            let n = Int(e.getIntegerValueField(.mouseEventButtonNumber))
            let (sw, ed) = router.triggerUp(.mouse(n))
            if let ed { edge(ed) }
            if sw { return nil }
            lastButtonAt = now
            return pass

        case .mouseMoved, .leftMouseDragged, .rightMouseDragged, .otherMouseDragged:
            let dx = Double(e.getIntegerValueField(.mouseEventDeltaX))
            let dy = Double(e.getIntegerValueField(.mouseEventDeltaY))
            let loc = e.location
            if let last = lastLoc, config.logLevel >= 2 {
                speedTravel += abs(loc.x - last.x) + abs(loc.y - last.y)
                speedDelta += abs(dx) + abs(dy)
                if now - speedSince > 0.5 {
                    if speedDelta > 20 {
                        Log.verbose(String(format: "ptrrate level %.2f desktop %.0f pt/s", zoom.level, speedTravel / (now - speedSince)))
                    }
                    speedSince = now; speedTravel = 0; speedDelta = 0
                }
            }
            lastLoc = loc
            lastMoveAt = now
            mouseDx += dx; mouseDy += dy
            pendingLockDx += dx; pendingLockDy += dy
            if zoom.level > 1.0001 || inspect { wake() }
            return pass

        case .scrollWheel:
            let continuous = e.getIntegerValueField(.scrollWheelEventIsContinuous) != 0
            let modsOK = config.wheelMods.map { modsMatch(testMods ?? mods, $0) } ?? false
            var steps: Double
            if continuous {
                // A trackpad gesture is decided once, when it begins, and its momentum follows that
                // decision: otherwise releasing the modifier mid-gesture splits it between zoom and app.
                let phase = e.getIntegerValueField(CGEventField(rawValue: 99)!)       // scroll phase
                let momentum = e.getIntegerValueField(CGEventField(rawValue: 123)!)   // momentum phase
                if momentum != 0 { return scrollGestureZooms ? nil : pass }
                if phase == 1 || phase == 128 || phase == 0 { scrollGestureZooms = modsOK }
                guard scrollGestureZooms else { return pass }
                steps = e.getDoubleValueField(.scrollWheelEventPointDeltaAxis1) / 12.0
            } else {
                guard modsOK else { return pass }
                // One notch is one step, as in Wind on Windows. macOS accelerates wheel deltas
                // (measured: a slow notch reports 0.1, quick notches ramp to 3.9), which made the
                // zoom crawl on slow scrolls and leap on fast ones.
                let line = e.getIntegerValueField(.scrollWheelEventDeltaAxis1)
                let fixed = e.getDoubleValueField(.scrollWheelEventFixedPtDeltaAxis1)
                steps = line != 0 ? (line > 0 ? 1 : -1) : (fixed > 0 ? 1 : fixed < 0 ? -1 : 0)
            }
            steps *= config.wheelZoomSpeed
            // Natural scrolling flips the reported direction; undo it so wheel/fingers up zooms in.
            if let ns = NSEvent(cgEvent: e), ns.isDirectionInvertedFromDevice { steps = -steps }
            if steps != 0 {
                zoom.wheelNotches(steps)
                wake()
            }
            return nil

        default:
            return pass
        }
    }

    private func edge(_ ed: InputRouter.Edge) {
        switch ed {
        case .down(.quickZoom):
            let r = applyQuickZoom(cur: zoom.level, stored: storedQuickLevel, def: config.quickZoomDefault, maxLevel: config.maxLevel)
            storedQuickLevel = r.newStored
            zoom.glide(to: r.newLevel)
        case .down(.inspect):
            inspect ? endInspect() : beginInspect()
        case .down(.hideCursor):
            // Like Wind's hideCursor hotkey: the pointer stays hidden until pressed again.
            userHidCursor.toggle()
            if !inspect && !pointerHiddenForCaret { SkyLight.setCursorHidden(userHidCursor) }
            Log.info("pointer \(userHidCursor ? "hidden" : "shown") by the hide-cursor bind")
        case .down(.quit):
            onQuitRequested?()
        default:
            break
        }
        Log.verbose("bind \(ed)")
        wake()
    }

    // MARK: inspect

    private func beginInspect() {
        guard zoom.level > 1.0001 else { return }
        inspect = true
        let p = Magnifier.pointer() ?? .zero
        inspectAnchor = p
        inspectRef = Pt(p.x, p.y)
        // The crosshair starts exactly where the pointer is, and the view does not move: it
        // follows the look point the same way it followed the pointer.
        inspectLook = Pt(p.x, p.y)
        SkyLight.setCursorHidden(true)
        Log.info("inspect on at \(Int(p.x)),\(Int(p.y))")
    }

    private func endInspect() {
        guard inspect else { return }
        inspect = false
        crosshair.show(at: nil, level: 1)
        DrawZoom.shared.crosshair = nil
        if !pointerHiddenForCaret && !userHidCursor { SkyLight.setCursorHidden(false) }
        lock.reset()                                // Wind's own hiding must not read as a game
        // The pointer comes to the crosshair (next tick), so the view stays where it is.
        if zoomedSession { pendingPlace = inspectLook }
        Log.info("inspect off at \(Int(inspectLook.x)),\(Int(inspectLook.y))")
    }

    // MARK: tick

    func wake() {
        if let l = link, l.isPaused { lastTick = 0; l.isPaused = false }
    }

    /// Test hook (A/B, `wcur delay N`): run the frame's work this many seconds after the display link
    /// fires. nil: the engine's default (`drawTickDelay` on the render engine, none on macOS's zoom).
    nonisolated(unsafe) static var tickDelay: Double?
    /// Render engine: the display link fires about when WindowServer composites, and a commit made
    /// then sometimes caught this refresh and sometimes the next: one refresh repeated, the next showed
    /// a double step (4.1-4.3% at speed 1.75, the owner's "steps of about 5%"; 33 such hitches in 12
    /// holds). Run 3 ms later the commit lands clear of the composite (2 in 8 holds, max step 2.2%).
    static let drawTickDelay = 0.003
    private var delayedTickPending = false
    private var delayedLink: CADisplayLink?
    private lazy var tickTimer: DispatchSourceTimer = {
        let t = DispatchSource.makeTimerSource(flags: .strict, queue: .main)
        t.setEventHandler { [weak self] in
            guard let self, let l = self.delayedLink else { return }
            self.delayedTickPending = false
            self.step(l)
        }
        t.resume()
        return t
    }()

    @objc private func tick(_ l: CADisplayLink) {
        let delay = Magnifier.tickDelay ?? (DrawZoom.enabled ? Magnifier.drawTickDelay : 0)
        if delay > 0 {
            if delayedTickPending { return }
            delayedTickPending = true
            delayedLink = l
            // A timer with no leeway: asyncAfter may coalesce and fire late, back into the composite.
            tickTimer.schedule(deadline: .now() + delay, leeway: .nanoseconds(0))
            return
        }
        step(l)
    }

    private func step(_ l: CADisplayLink) {
        let now = l.timestamp
        let dt = lastTick == 0 ? l.duration : min(0.1, now - lastTick)
        lastTick = now
        if paused { l.isPaused = true; return }

        zoom.direction = resolveDirection(inHeld: held(.zoomIn), outHeld: held(.zoomOut))
        // Render engine, cold start (first zoom, or after a minute at 1x stopped the capture): hold the
        // level at 1x until a captured frame is at hand, so the overlay shows the zoom from its first
        // step. Zooming on meanwhile made the overlay pop in at 1.05-1.14 after 90-136 ms with no cursor
        // (2026-10-31). The display link keeps running; lastTick moves on, so no stalled dt is integrated.
        if DrawZoom.enabled && !zoomedSession && zoom.level <= 1.0001 && zoom.wantsZoomIn && !DrawZoom.shared.hasFrame {
            DrawZoom.shared.prewarm()
            if config.lockTracking { _ = raw.start() }  // its first open stalls a tick: here, not mid-zoom
            if DrawZoom.enabled { return }              // a failed start falls through to macOS's zoom
        }
        zoom.tick(dt)
        SkyLight.traceWrites = config.logLevel >= 3
        let lv = zoom.level

        // A new level is written first, before the pointer is read and before any placement.
        // Written after a placement, a level write sometimes undid it (the pointer snapped back the
        // whole placed shift), and WindowServer applies queued hand motion during it; written here,
        // both land before the read and the frame's placement runs at a steady level, as at a fixed
        // zoom (measured 2026-10-30: the pan slowed 23% and stepped back while zooming). At a screen
        // edge the view is written at the clamp for the NEW level, so a pointer on the edge stays
        // inside the view.
        var levelWritten = false
        if zoomedSession && lv > 1.0001 && abs(lv - liveLevel) > 1e-6 && owner.owner == .mouse && !inspect && !lock.locked {
            // A view pinned to an edge stays pinned to the NEW level's edge: last frame's centre (written
            // a point past the old clamp) is inside the new clamp when zooming in, and the pointer on
            // the edge was pushed inward at every step (traced 2026-10-30: 0.04 -> 0.48 pt).
            let ol = max(1, liveLevel)
            func pin(_ v: Double, _ size: Double) -> Double {
                let oh = size / ol / 2, nh = size / lv / 2
                guard config.mouseAlign == 0 else { return v }   // edge pan: the pointer may sit deep in the view
                if v <= oh + 1e-6 { return nh }
                if v >= size - oh - 1e-6 { return size - nh }
                return v
            }
            let c = clampCenter(Pt(pin(liveCenter.x, screen.w), pin(liveCenter.y, screen.h)), level: lv, screen: screen)
            // At a pinned edge too the level write keeps the pointer's desktop spot: the view clamps
            // per axis and the cursor glides off centre and back as the level changes, exactly as Wind
            // on Windows does (it never moves the pointer; recording 2026-10-31). Keeping the screen
            // spot instead (tried 2026-10-31) slid the pointer off what it was on.
            send(c, level: lv, keepPointer: true)
            lastWritten = (c, lv)
            levelWritten = true
        }

        // The pointer is read ONCE per tick, here; every later step works from this read and from
        // the whole points Wind's own carries move it, never from a read after a write. Right after
        // a level write the first read can be tens of points off for about 0.5 ms, then holds
        // (PROBE at the bottom edge, 2026-10-30: 989.08 then 1082.88 x5): read until two agree.
        var ptr = Magnifier.pointer() ?? CGPoint(x: center.x, y: center.y)
        if levelWritten {
            for _ in 0..<3 {
                usleep(500)
                guard let q = Magnifier.pointer() else { break }
                if abs(q.x - ptr.x) + abs(q.y - ptr.y) < 0.05 { break }
                ptr = q
            }
        }
        let ldx = pendingLockDx, ldy = pendingLockDy
        pendingLockDx = 0; pendingLockDy = 0
        wroteThisTick = false

        // Holding zoom-out at 1x is not a zoom.
        let moving = zoom.isMoving && !(zoom.direction == .zoomOut && lv <= 1.0001 && !zoom.hasTarget)
        if lv <= 1.0001 && !moving {
            if zoomedSession {
                SkyLight.reset()
                liveLevel = 1
                zoomedSession = false
                caret.zoomed = false
                lastWritten = nil
                endInspect()
                windCursor.show(at: nil, on: false); windCursorAt = nil
                clearSessionFlags()
                Log.info("zoom session end")
            }
            mouseDx = 0; mouseDy = 0
            reportLevel()
            l.isPaused = true
            return
        }

        if !zoomedSession {
            zoomedSession = true
            caret.zoomed = true
            updateScreen()
            center = clampCenter(Pt(ptr.x, ptr.y), level: lv, screen: screen)
            virtualPtr = Pt(ptr.x, ptr.y)
            springVel = (0, 0)
            clearSessionFlags()
            lastPtr = Pt(ptr.x, ptr.y)
            resetViewOwnerForSession(&owner)
            lock.reset()
            Log.info("zoom session start at \(Int(ptr.x)),\(Int(ptr.y))")
        }

        tickStartCenter = center
        placedLastTick = placedThisTick; placedThisTick = Pt(0, 0); carryView = nil
        glideFrame = false
        // A game holding the pointer: the cursor stays hidden while the hand keeps moving. While it
        // holds it, pan from raw HID counts: Wind's own view writes drag the held pointer around and
        // WindowServer reports that as moves with large unrelated deltas, so tap deltas run away.
        let rawOn = (config.lockTracking || inspect) && raw.start()
        let r = raw.take()
        let motion = rawOn ? abs(r.dx) + abs(r.dy) : abs(ldx) + abs(ldy)
        if rawOn && motion > 0 { lastRawAt = CACurrentMediaTime() }   // raw counts are flowing (a trackpad sends none)
        // A read far beyond what the hand did is held back for a frame: near screen edges, while the
        // level changes, a read can be tens of points off for a few ms (the explorer, 2026-10-30).
        // Read there twice in a row, the pointer really is there (a warp, macOS undoing a
        // placement) and it is taken.
        if let prev = lastPtr, zoomedSession, lv > 1.0001, owner.owner == .mouse, !inspect, !lock.locked,
           !DrawZoom.enabled, rawOn, CACurrentMediaTime() - lastRawAt < 1 {
            let far = ((ptr.x - prev.x) * (ptr.x - prev.x) + (ptr.y - prev.y) * (ptr.y - prev.y)).squareRoot() * lv
                > 24 + 8 * (motion + prevRawMotion)
            if far, !(pendingJump.map { abs($0.x - ptr.x) + abs($0.y - ptr.y) < 2 } ?? false) {
                Log.verbose(String(format: "pointer read %.1f,%.1f held back (last %.1f,%.1f)", ptr.x, ptr.y, prev.x, prev.y))
                pendingJump = Pt(ptr.x, ptr.y)
                ptr = CGPoint(x: prev.x, y: prev.y)
            } else { pendingJump = nil }
        }
        defer { prevRawMotion = motion; prevRawStep = rawStep }   // counts can land a frame after the pointer moves
        rawStep = rawOn && CACurrentMediaTime() - lastRawAt < 1 ? Pt(r.dx, r.dy) : nil
        let decay = exp(-dt / 0.02)                    // pending pushes fade over ~20 ms, any frame rate
        pushX *= decay; pushY *= decay; if abs(pushX) < 0.05 { pushX = 0 }; if abs(pushY) < 0.05 { pushY = 0 }
        redoAge += 1; if redoAge > 3 { redoX = 0; redoY = 0 }
        // The pointer's own movement since the end of the last tick (after Wind's writes), so a
        // pointer that follows the hand never counts as held.
        // While a game holds the pointer's screen position, each view shift drags its desktop
        // position by exactly that shift (applied after the write returns, or not); that part is
        // Wind's own and must not read as the pointer following the hand.
        // The drag can land up to two ticks late (measured), so the last two shifts are discounted.
        let moved = afterWrites.map { a -> Double in
            let mx = ptr.x - a.x, my = ptr.y - a.y
            // (Draw engine: the view does not drag the pointer, so no shift is discounted.)
            let candidates = DrawZoom.enabled ? [Pt(0, 0)] : [Pt(0, 0), lastShift, prevShift, Pt(lastShift.x + prevShift.x, lastShift.y + prevShift.y)]
            return candidates.map { abs(mx - $0.x) + abs(my - $0.y) }.min() ?? 0
        } ?? 0
        // In screen points: at a high zoom a slow hand moves the pointer a tiny fraction of a desktop
        // point per frame, which read as held and flipped a false game lock on and off (it pulled
        // the view into a corner, owner recording 2026-10-30).
        // A pointer pushed against a screen edge stays put while the hand moves, exactly like one a
        // game holds: that is no evidence (every false lock in the owner's recording, 2026-10-30,
        // began with the pointer at x = 0 and macOS reporting the cursor hidden).
        let atEdge = ptr.x < 2 || ptr.y < 2 || ptr.x > screen.w - 3 || ptr.y > screen.h - 3
        // Draw engine and Wind's own cursor: Wind hides the real cursor itself, so "hidden" cannot tell a game. A game holds
        // the pointer: it does not move at all for several ticks while counts keep coming.
        drawHeldTicks = motion > 0 && moved < 0.01 && !atEdge ? drawHeldTicks + 1 : (moved >= 0.01 ? 0 : drawHeldTicks)
        drawFreeTicks = motion > 0 && moved >= 0.01 ? drawFreeTicks + 1 : (motion > 0 ? 0 : drawFreeTicks)
        // Once locked, it takes a few ticks of the pointer following the hand to let go: on macOS's
        // zoom a view shift drags the held pointer, sometimes late, and one such tick let go of a
        // game (Wind's own cursor, 2026-10-31).
        let heldFree = lock.locked ? drawFreeTicks >= 3 : drawHeldTicks < 6
        if !inspect && !userHidCursor { lock.feed(cursorVisible: DrawZoom.enabled || SkyLight.forceHidden ? heldFree : SkyLight.cursorVisible, motion: atEdge && !lock.locked ? 0 : motion, pointerMoved: moved * max(lv, 1)) }  // Inspect hides it itself
        if config.logLevel >= 2 && (motion > 0 || lock.locked) {
            Log.verbose(String(format: "lockdbg vis %d motion %.0f moved %.2f shift %.2f locked %d", SkyLight.cursorVisible ? 1 : 0, motion, moved, lastShift.x, lock.locked ? 1 : 0))
        }
        if lock.locked && !rawOn && config.lockTracking && !askedRawAccess {
            askedRawAccess = true
            Log.info("game lock: asking for Input Monitoring to follow the mouse in games")
            RawMouse.requestAccess()
        }

        // Wobble metric (diagnostics): where the real pointer sits on screen relative to where the
        // last view put it, in screen points, at the start of this frame.
        if config.logLevel >= 2, let w = lastWritten, owner.owner == .mouse, !inspect {
            Log.verbose(String(format: "wobble %.2f %.2f lv %.2f", (ptr.x - w.c.x) * w.level, (ptr.y - w.c.y) * w.level, w.level))
        }

        // Who owns the view this tick.
        let locked = config.lockTracking && lock.locked
        if wasLocked && !locked { returning = true; returnOffset = nil }
        if locked && !wasLocked { virtualPtr = center }   // keep panning from what is on screen
        if locked != wasLocked { Log.info(locked ? "game lock: panning from mouse deltas" : "game lock released") }
        wasLocked = locked
        var oi = ViewOwnerInputs()
        oi.enabled = !locked && !inspect
        oi.trackCaret = config.trackCaret
        oi.trackFocus = config.trackFocus
        oi.mouseDx = mouseDx; oi.mouseDy = mouseDy
        oi.buttonDown = [CGMouseButton.left, .right, .center].contains {
            CGEventSource.buttonState(.combinedSessionState, button: $0)
        }
        let nowT = CACurrentMediaTime()
        oi.msSinceButton = (nowT - lastButtonAt) * 1000
        oi.msSinceKey = (nowT - lastKeyAt) * 1000
        oi.keyAfterButton = keyAfterButton
        oi.dtMs = dt * 1000
        oi.snap = caret.snapshot
        let panX = (held(.panRight) ? 1.0 : 0) - (held(.panLeft) ? 1.0 : 0)
        let panY = (held(.panDown) ? 1.0 : 0) - (held(.panUp) ? 1.0 : 0)
        oi.panning = panX != 0 || panY != 0
        oi.trackActive = config.trackCaret || config.trackFocus
        mouseDx = 0; mouseDy = 0
        let prevOwner = owner.owner
        stepViewOwner(&owner, oi)
        if owner.owner != prevOwner { Log.info("owner \(prevOwner) -> \(owner.owner)\(owner.warpPointer ? " (pointer comes to the view)" : "")") }
        // While the caret or focus owns the view, macOS drags the zoomed pointer along the view's
        // edge (it cannot sit outside the view). Hide it until the mouse takes the view back,
        // when it reappears at the view centre (owner request 2026-10-30).
        let hideForCaret = config.hidePointerWhileTracking && (owner.owner == .caret || owner.owner == .focus)
        if hideForCaret != pointerHiddenForCaret {
            pointerHiddenForCaret = hideForCaret
            if !inspect && !userHidCursor { SkyLight.setCursorHidden(hideForCaret) }
        }
        // Text views (TextEdit) show the cursor again as they handle keys: hide it again.
        if (pointerHiddenForCaret || userHidCursor) && !inspect && SkyLight.cursorVisible { SkyLight.setCursorHidden(true, force: true) }

        var target = center
        if inspect {
            // The real pointer stays frozen and hidden (a tooltip stays open); the hand moves the
            // crosshair (the look point) at 1x speed and the view follows it like the pointer.
            // Movement is measured from where the pointer really landed last tick (placement snaps
            // to a grid), so the snap is never counted as hand movement.
            let a = Pt(inspectAnchor.x, inspectAnchor.y)
            let sens = config.cursorSensitivity
            if rawOn {
                // Raw counts, like Wind's Inspect on Windows: independent of the held pointer.
                inspectLook = Pt(inspectLook.x + r.dx * sens, inspectLook.y + r.dy * sens)
                let landed = inspectRef ?? a
                if abs(ptr.x - landed.x) + abs(ptr.y - landed.y) > 0.5 { inspectRef = placePointer(at: a, from: Pt(ptr.x, ptr.y), level: lv, view: center) }
            } else {
                let ref = inspectRef ?? a
                if abs(ptr.x - ref.x) + abs(ptr.y - ref.y) > 0.001 {
                    inspectLook = Pt(inspectLook.x + (ptr.x - ref.x) * lv * sens, inspectLook.y + (ptr.y - ref.y) * lv * sens)
                    inspectRef = placePointer(at: a, from: Pt(ptr.x, ptr.y), level: lv, view: center)
                }
            }
            inspectLook = Pt(min(max(inspectLook.x, 0), screen.w - 1), min(max(inspectLook.y, 0), screen.h - 1))
            if SkyLight.cursorVisible { SkyLight.setCursorHidden(true, force: true) }   // the Dock can show it again
            target = mouseTarget(inspectLook, lv)
        } else if locked {
            // Without Input Monitoring there is no trustworthy hand motion: hold the view still.
            let gx = rawOn ? r.dx : 0, gy = rawOn ? r.dy : 0
            virtualPtr = Pt(virtualPtr.x + gx * config.cursorSensitivity, virtualPtr.y + gy * config.cursorSensitivity)
            virtualPtr = Pt(min(max(virtualPtr.x, 0), screen.w), min(max(virtualPtr.y, 0), screen.h))
            target = mouseTarget(virtualPtr, lv)
        } else {
            // Leaving Inspect places the pointer on the next tick; if another owner holds the view
            // then (a pan key still down), the placement is dropped rather than replayed later.
            if pendingPlace != nil && owner.owner != .mouse { pendingPlace = nil }
            switch owner.owner {
            case .mouse:
                // The mouse moved while the view showed the caret, keys or Inspect: the pointer
                // comes to the view (Wind's rule). A button press instead gives the view back to
                // the pointer, which glides there.
                if owner.warpPointer { pointerToView = true }
                else if prevOwner != .mouse { returning = true; returnOffset = nil }
                virtualPtr = Pt(ptr.x, ptr.y)
                let onZoomedDisplay = ptr.x >= 0 && ptr.y >= 0 && ptr.x < screen.w && ptr.y < screen.h
                if !onZoomedDisplay {
                    pointerToView = false               // never drag the pointer across displays
                    handTarget = nil
                } else if let p = pendingPlace {
                    pendingPlace = nil
                    returning = false; returnOffset = nil
                    virtualPtr = placePointer(at: p, from: virtualPtr, level: lv, view: center)
                    handTarget = nil
                } else if pointerToView {
                    pointerToView = false
                    returning = false; returnOffset = nil
                    virtualPtr = placePointer(at: center, from: virtualPtr, level: lv, view: center)
                    handTarget = nil
                } else if DrawZoom.enabled {
                    // Drawn zoom: macOS is not zoomed, the pointer already moves at 1x.
                    handTarget = nil
                } else if config.keepPointerSpeed && !returning && slowPanGlide(ptr, lv, dt, r) {
                    // handled: the pointer and view glide by WindowServer (issue #13)
                } else if let prev0 = lastPtr, lv > 1.0001, let prev = undonePlacement(prev0, ptr),
                          config.keepPointerSpeed && !returning {
                    // macOS undid last tick's placement: the pointer is back where it was, give or
                    // take the hand. Measure the hand from there and keep the target, so the next
                    // placement redoes it (recorded 2026-10-30: at a screen edge a 1 pt undo, times
                    // the level, snapped the pointer back 17 pt and then forward).
                    if handTarget == nil { handTarget = prev0 }
                    virtualPtr = compensatePointerSpeed(from: prev, now: virtualPtr, level: lv, dt: dt)
                } else if rawOn, nowT - lastRawAt < 1, motion + prevRawMotion == 0 {
                    // The raw counts say the hand is still (this and last frame; counts can land a
                    // frame late): whatever moved the pointer, it was not the hand. Take it as it is,
                    // never times the level (2026-10-30: at the right edge the read flipped
                    // 1920.000 / 1919.15 every frame while zooming with the hand still). The target is
                    // kept while the pointer is near it: dropping it recentred the view on the pointer,
                    // up to a point behind, which stepped back a point mid-pan whenever a frame got no
                    // counts (e2e 'zooming while panning', 2026-10-30).
                    if let t = handTarget, abs(t.x - ptr.x) > 2 || abs(t.y - ptr.y) > 2 { handTarget = nil }
                } else if rawOn, nowT - lastRawAt < 1, let prev = lastPtr, lv > 1.0001,
                          ((ptr.x - prev.x) * (ptr.x - prev.x) + (ptr.y - prev.y) * (ptr.y - prev.y)).squareRoot() * lv > 24 + 8 * (motion + prevRawMotion) {
                    // The pointer jumped further than the hand moved (raw counts): something else
                    // moved it, like macOS undoing a placement after typing (recorded 2026-10-30:
                    // an 82 pt snap-back on 1 count, times the level, flung the view across the
                    // screen). Take it as it is, never as hand motion.
                    handTarget = nil
                    Log.info(String(format: "pointer jumped %.0f,%.0f -> %.0f,%.0f on %.0f raw counts: not hand motion", prev.x, prev.y, ptr.x, ptr.y, motion))
                } else if config.keepPointerSpeed && !returning, let prev = lastPtr {
                    let stepAt1x = ((ptr.x - prev.x) * (ptr.x - prev.x) + (ptr.y - prev.y) * (ptr.y - prev.y)).squareRoot() * lv
                    let k = exp(-dt / 0.08)
                    handSpeedAvg = handSpeedAvg * k + stepAt1x / max(dt, 0.001) * (1 - k)
                    if config.panSmoothMs > 0 && lv > 1.0001 {
                        virtualPtr = glidePointer(from: prev, now: virtualPtr, level: lv, dt: dt)
                    } else {
                        virtualPtr = compensatePointerSpeed(from: prev, now: virtualPtr, level: lv, dt: dt)
                    }
                } else {
                    handTarget = nil
                }
                // The view follows the pointer, so the pointer stays locked at the screen centre.
                // Following the hand's exact path instead left the pointer up to a point off centre,
                // a level-pixel wobble (owner 2026-10-31). The uneven whole-point steps that caused
                // the shake while zooming are evened out where the pointer is placed (compensation).
                var want = mouseTarget(virtualPtr, lv)
                if glideFrame { want = liveCenter }        // a glide owns the view: no other write
                // Small corrections to the target (a bounded read, a discounted push) can pull it back a
                // fraction across a whole point, which showed as a one-point back-step against the pan.
                // Against the way the raw counts say the hand is going, the view holds instead.
                // Not where the target is a screen edge's clamp: zooming, the clamp itself moves (out
                // toward the edge when zooming in), and holding against it left the view a fraction
                // inside the new clamp while the next level write pinned it again; at a pinned edge
                // while zooming and panning the view went back and forth every frame (owner 2026-10-31:
                // "near edges way more shaking").
                if let rs = rawStep, !returning, !DrawZoom.enabled {   // whole-point zoom only
                    let dir = Pt(rs.x != 0 ? rs.x : (prevRawStep?.x ?? 0), rs.y != 0 ? rs.y : (prevRawStep?.y ?? 0))
                    let hw = screen.w / lv / 2, hh = screen.h / lv / 2
                    let edgeX = want.x <= hw + 1e-6 || want.x >= screen.w - hw - 1e-6
                    let edgeY = want.y <= hh + 1e-6 || want.y >= screen.h - hh - 1e-6
                    if !edgeX && dir.x != 0 && (want.x - center.x) * dir.x < 0 && abs(want.x - center.x) < 1.5 { want.x = center.x }
                    if !edgeY && dir.y != 0 && (want.y - center.y) * dir.y < 0 && abs(want.y - center.y) < 1.5 { want.y = center.y }
                }
                // After a carry the view already sits where it should, on whole points. A
                // keepPointer=true write that moved the floored centre after the carry, in the same
                // frame, left the next read off (8% of such frames against 0.4%, Wind logs
                // 2026-10-30): taken as hand motion times the level, it threw the view 51 screen pt
                // and back 36 on a slow diagonal. So the view stays on the carry's centre, on every
                // axis the view is not held at a screen edge.
                if let b = carryView, !returning {
                    let hw = screen.w / lv / 2, hh = screen.h / lv / 2
                    if want.x > hw + 1e-6 && want.x < screen.w - hw - 1e-6 && abs(want.x - b.x) < 1 { want.x = b.x }
                    if want.y > hh + 1e-6 && want.y < screen.h - hh - 1e-6 && abs(want.y - b.y) < 1 { want.y = b.y }
                }
                if returning {
                    // The view glides back from where it was (the caret, keys, a game) by gliding the
                    // OFFSET between it and the pointer's view to zero, while that view follows the
                    // pointer directly. Springing the view itself toward a moving pointer lagged behind
                    // it for as long as the hand moved and never finished: after typing, panning drew
                    // the cursor 70-90 screen pt off centre, swinging with the hand's speed (owner
                    // 2026-10-31: "after typing the cursor tracking degenerates").
                    resetSmooth()
                    let o0 = returnOffset ?? Pt(center.x - want.x, center.y - want.y)
                    var vx = springVel.x, vy = springVel.y
                    let ox = springToward(o0.x, 0, vel: &vx, dtMs: dt * 1000, glideMs: 160)
                    let oy = springToward(o0.y, 0, vel: &vy, dtMs: dt * 1000, glideMs: 160)
                    springVel = (vx, vy)
                    returnOffset = Pt(ox, oy)
                    target = Pt(want.x + ox, want.y + oy)
                    if abs(ox) + abs(oy) < 0.5 { returning = false; returnOffset = nil; springVel = (0, 0) }
                } else {
                    target = want
                    returnOffset = nil
                    springVel = (0, 0)
                    // Render engine: pan smoothing gives the POINTER weight against hand jitter, which the
                    // zoom magnifies. Adaptive (One Euro): slow, small motion is smoothed with the time
                    // constant set; real motion raises the cutoff with its speed. The drawn cursor and the
                    // view follow the smoothed pointer, so the cursor stays centred (smoothing the view
                    // alone left the cursor off centre while moving: owner 2026-11-01), and clicks are
                    // moved to it (the tap), so they land where the cursor shows. The real
                    // pointer moves natively: a warp would put it back on whole points.
                    // (On macOS's zoom the setting glides the pointer instead.)
                    if DrawZoom.enabled && config.panSmoothMs > 0 && lv > 1.0001 {
                        let fc = 1 / (2 * Double.pi * config.panSmoothMs / 1000)
                        euroX.minCutoff = fc; euroY.minCutoff = fc
                        let fp = Pt(min(max(euroX.filter(virtualPtr.x, dt: dt), 0), screen.w - 1),
                                    min(max(euroY.filter(virtualPtr.y, dt: dt), 0), screen.h - 1))
                        smoothPointer = fp
                        target = mouseTarget(fp, lv)
                    } else { resetSmooth() }
                }
            case .caret, .focus:
                resetSmooth()
                if let t = trackTargetCenter(owner.target.rect, cur: center, level: lv, screen: screen,
                                             align: config.trackAlign, marginPct: config.trackMarginPct) {
                    var vx = springVel.x, vy = springVel.y
                    let x = springToward(center.x, t.x, vel: &vx, dtMs: dt * 1000, glideMs: config.trackGlideMs)
                    let y = springToward(center.y, t.y, vel: &vy, dtMs: dt * 1000, glideMs: config.trackGlideMs)
                    springVel = (vx, vy)
                    target = Pt(x, y)
                }
            case .keys:
                resetSmooth()
                let v = keyPanSpeed(level: lv, screenWidth: screen.w, panSpeed: config.panSpeed) * dt
                target = Pt(center.x + panX * v, center.y + panY * v)
            }
        }
        center = clampCenter(target, level: lv, screen: screen)
        if config.logLevel >= 2 { Log.verbose(String(format: "view %.4f %.4f ptr %.4f %.4f lv %.5f", center.x, center.y, ptr.x, ptr.y, lv)) }
        // The crosshair window moves first and its move is committed before the zoom write, so both
        // land in the same frame; moved after, it trailed the view by a frame while panning.
        crosshairAt = inspect ? crosshairPoint(lv, centre: center) : nil
        if DrawZoom.enabled {                           // render draws it with the view (DrawZoom.crosshair)
            DrawZoom.shared.crosshair = crosshairAt.map { CGPoint(x: $0.x, y: $0.y) }
            crosshair.show(at: nil, level: lv)
        } else {
            crosshair.show(at: crosshairAt, level: lv)
        }
        // Wind's own cursor (cursor = "wind"), moved before the write like the crosshair.
        let ownCursor = config.cursor == "wind" && !DrawZoom.enabled
        let pNow = owner.owner == .mouse && !locked && !inspect ? virtualPtr : Pt(ptr.x, ptr.y)
        // Following the pointer, the exact centre is the pointer's own (a placement tick writes it at
        // a .5 centre instead, which put the cursor half a point off every other frame).
        let follow = owner.owner == .mouse && !locked && !inspect && config.mouseAlign == 0
        let exact = follow ? clampCenter(mouseTarget(pNow, lv), level: lv, screen: screen) : center
        let drawnAt = ownCursor && !inspect && !locked && !SkyLight.wantHidden ? cursorPoint(lv, centre: center, exact: exact, pointer: pNow) : nil
        windCursorAt = drawnAt
        windCursorOffset = drawnAt == nil ? nil : Pt(pNow.x - exact.x, pNow.y - exact.y)
        windCursor.show(at: drawnAt, on: ownCursor)
        if config.logLevel >= 2, let d = drawnAt {
            Log.verbose(String(format: "wcur %.4f %.4f ptr %.4f %.4f", d.x, d.y, pNow.x, pNow.y))
        }
        write(lv)
        // A plain read after the writes, for the game-lock detector only (a held pointer is dragged
        // by Wind's writes). It is never used to measure the hand.
        let endPtr = Magnifier.pointer().map { Pt($0.x, $0.y) }
        if !(owner.owner == .mouse && !locked && !inspect) { smoothPointer = nil }
        // Render: the cursor is drawn at the pointer the view was placed for this tick (the smoothed one,
        // or this tick's read), not at a later read. A read after the writes includes the hand's motion
        // since, and drew the cursor off the view centre by that much: 10-25% of frames jumped over half a
        // point while panning at 4x-12x (cursor_truth on render, 2026-11-01).
        let following = owner.owner == .mouse && !locked && !inspect
        if DrawZoom.enabled, let e = smoothPointer ?? (following ? virtualPtr : endPtr) { DrawZoom.shared.lockHidden = locked; DrawZoom.shared.updateCursor(CGPoint(x: e.x, y: e.y)) }
        prevShift = lastShift
        lastShift = Pt(center.x - tickStartCenter.x, center.y - tickStartCenter.y)
        // Where Wind's writes left the pointer: this tick's read plus the whole points carried
        // (keepPointer=true writes keep its desktop position). Next tick the hand is measured
        // against it, so motion that landed during the writes counts once, at the zoomed gain.
        lastPtr = (owner.owner == .mouse && !locked && !inspect) ? virtualPtr : nil
        // Where the view is held at a screen edge, the frame's last write puts it back on the clamp
        // after the carry and the carry comes out short: the model is wrong on that axis, so the
        // next frame measures from where the pointer really is (unless the read is a transient,
        // more than 3 points off). Measured 2026-10-30: corner -> centre snapped back ~10 pt.
        if var m = lastPtr, let e = endPtr, rawStep != nil, !glideFrame {
            let hw = screen.w / lv / 2, hh = screen.h / lv / 2
            if (center.x <= hw + 1e-6 || center.x >= screen.w - hw - 1e-6) && abs(e.x - m.x) < 2 { m = Pt(e.x, m.y) }
            if (center.y <= hh + 1e-6 || center.y >= screen.h - hh - 1e-6) && abs(e.y - m.y) < 2 { m = Pt(m.x, e.y) }
            lastPtr = m
        }
        afterWrites = endPtr
        reportLevel()

        // Sleep while nothing changes; mouse moves, binds, the wheel and caret events wake it.
        let busy = wroteThisTick || moving || returning || oi.panning || abs(springVel.x) + abs(springVel.y) > 0.01
            || held(.zoomIn) || held(.zoomOut)
        idleTicks = busy ? 0 : idleTicks + 1
        if idleTicks > 30 { idleTicks = 0; l.isPaused = true }
    }

    /// Where Inspect's crosshair is drawn: the centre the zoom really shows. The view moves in whole
    /// desktop points, so a crosshair at the exact look point wobbled around the screen centre by up
    /// to a point times the level while panning (owner report 2026-10-30, as fixed in Wind on
    /// Windows). Only where the view is held at a screen edge does it leave the centre.
    /// Where Wind's own cursor is drawn: where the pointer would be on screen if the zoom showed the
    /// exact (fractional) centre. The zoom shows the whole centre, so the cursor is moved by the
    /// fraction it drops; on a view that follows the pointer it then stays put on screen.
    private func cursorPoint(_ lv: Double, centre: Pt, exact e: Pt, pointer p: Pt) -> CGPoint {
        func axis(_ c: Double, _ e: Double, _ p: Double, _ size: Double) -> Double {
            let half = size / lv / 2
            let shown = min(max(floor(writeAxis(c, half, size)), half), size - half)
            return p - e + shown
        }
        return CGPoint(x: axis(centre.x, e.x, p.x, screen.w), y: axis(centre.y, e.y, p.y, screen.h))
    }

    /// Before a view write: Wind's cursor goes where that write shows it (the whole centre the zoom
    /// will show, clamped like the compositor clamps it, plus the cursor's last offset from it).
    /// Inspect's crosshair likewise goes to the whole centre that write shows.
    private func cursorBeforeWrite(_ c: CGPoint, scale: Double) {
        let lv = max(1, scale)
        if crosshairAt != nil, crosshair.drawnAt != nil {
            func axis(_ c: Double, _ look: Double, _ size: Double) -> Double {
                let half = size / lv / 2, shown = floor(c)
                return shown >= half && shown <= size - half ? shown : look
            }
            crosshair.move(to: CGPoint(x: axis(c.x, inspectLook.x, screen.w), y: axis(c.y, inspectLook.y, screen.h)), level: lv)
        }
        guard let o = windCursorOffset, windCursor.active, windCursor.drawnAt != nil else { return }
        func axis(_ c: Double, _ size: Double) -> Double {
            let half = size / lv / 2
            return min(max(floor(c), half), size - half)
        }
        windCursor.move(to: CGPoint(x: axis(c.x, screen.w) + o.x, y: axis(c.y, screen.h) + o.y))
    }

    private func crosshairPoint(_ lv: Double, centre: Pt) -> CGPoint {
        // Draw engine: the view shows the exact (fractional) centre, which is the look point except
        // where the view stops at a screen edge; the crosshair stays on what Inspect looks at.
        if DrawZoom.enabled { return CGPoint(x: inspectLook.x, y: inspectLook.y) }
        func axis(_ c: Double, _ look: Double, _ size: Double) -> Double {
            let half = size / lv / 2, shown = floor(writeAxis(c, half, size))
            return shown >= half && shown <= size - half ? shown : look
        }
        return CGPoint(x: axis(centre.x, inspectLook.x, screen.w), y: axis(centre.y, inspectLook.y, screen.h))
    }

    private func clearSessionFlags() {
        handTarget = nil; ptrVel = (0, 0)
        pushX = 0; pushY = 0; redoX = 0; redoY = 0; rawStep = nil; prevRawStep = nil; pendingJump = nil
        rawMismatch = 0; rawDistrusted = false
        glideUntil = 0; glideMode = false; glideFrame = false
        if pointerHiddenForCaret { pointerHiddenForCaret = false; if !inspect && !userHidCursor { SkyLight.setCursorHidden(false) } }
        afterWrites = nil
        pointerToView = false
        pendingPlace = nil
        returning = false; returnOffset = nil
        wasLocked = false
        inspectRef = nil
        idleTicks = 0
    }

    /// While zoomed, WindowServer moves the pointer in SCREEN space, so a hand movement crosses
    /// only 1/level of the desktop it would at 1x (measured with a virtual HID mouse 2026-10-29:
    /// 68.8 pt at 1x, 17.2 at 4x, 6.9 at 10x). Wind keeps the 1x desktop speed: move the view
    /// with the screen position kept (the pointer rides along the missing (level-1) part of the
    /// step), then the normal write (desktop position kept) recentres. Rewriting event locations
    /// or warping instead is reinterpreted by the zoom and snaps the pointer.
    private func compensatePointerSpeed(from prev: Pt, now p: Pt, level lv: Double, dt: Double) -> Pt {
        var dx = p.x - prev.x, dy = p.y - prev.y
        var withHand = Pt(0, 0)                          // clipped motion with the hand, taken 1:1
        // Bounded by the hand: on each axis a step can be no bigger on screen than the raw counts of
        // this and the last tick allow (macOS moves ~0.7 pt per count, up to ~3 with acceleration).
        // Anything more is Wind's own carry landing a point off its model, and multiplied by the
        // level it snapped the view back and forth (explorer 2026-10-30: +-5 pt at 7.3x on 0-4 counts).
        // Raw counts that keep explaining less motion than the pointer made, with the hand, are not
        // trusted: a run where the counts arrived incompletely clipped real motion to 61% speed.
        if rawStep != nil, rawMismatch > 0.35 {
            rawStep = nil
            rawMismatch *= exp(-dt / 0.5)                // try the counts again after a while
            if rawMismatch <= 0.35 { rawDistrusted = false; Log.info("raw counts trusted again") }
        }
        if let a = rawStep, lv > 1.0001 {
            let b = prevRawStep ?? Pt(0, 0)
            // Signed: the counts also say which way the hand went; against them only a hair passes
            // (leaving the right edge, a read 3.5 pt the wrong way snapped the view 17 pt back).
            // A push against the counts usually snaps back a frame or two later: that return is not
            // the hand either, so it is taken off the pending push first (it read as a 19 pt fling).
            // Each tick's counts are bounded on their own (a quick reversal must not cancel them
            // out), up to 6 pt per count with the hand; excess with the hand is dropped, not kept.
            // A carry macOS undid and then re-applied a tick later is taken off only when the step
            // matches it (a looser rule ate genuine hand motion; review 2026-10-30).
            func bound(_ d: Double, _ a: Double, _ b: Double, _ pend: inout Double, _ redo: inout Double, _ excess: inout Double) -> Double {
                var d = d
                if redo != 0 && d * redo > 0 && abs(d - redo) <= 0.5 + 0.3 * abs(redo) { d -= redo; redo = 0 }
                if d * pend < 0 {                                   // moving back the way it was pushed
                    let take = d > 0 ? min(d, -pend) : max(d, -pend)
                    d -= take; pend += take
                }
                // The larger of this and the last tick's counts (they can land a tick late), learnt
                // points per count with 2x headroom: summed counts with 2.5x let a 0.4 pt carry error
                // through as a 12 screen pt snap at 30x.
                let k = min(6, max(1.2, 2 * ptsPerCount))
                let lo = (k * min(0, a, b) - 2) / lv, hi = (k * max(0, a, b) + 2) / lv
                let kept = min(max(d, lo), hi)
                let against = (a + b) == 0 ? 0 : (d - kept) * (a + b) < 0 ? d - kept : 0
                pend += against
                // Over the bound but with the hand: the hand sped up faster than the learnt rate. It
                // is taken as macOS moved it (1:1, not times the level), so the target never falls
                // behind the pointer and the carry never pulls it back.
                excess = d - kept - against
                return kept
            }
            dx = bound(dx, a.x, b.x, &pushX, &redoX, &withHand.x); dy = bound(dy, a.y, b.y, &pushY, &redoY, &withHand.y)
            let over = (abs(withHand.x) + abs(withHand.y)) * lv > 2 ? 1.0 : 0.0
            rawMismatch += (over - rawMismatch) * 0.1
            if rawMismatch > 0.35 && !rawDistrusted { rawDistrusted = true; Log.info("raw counts explain too little pointer motion: hand bound off") }
            // Learn how far the pointer moves per count (macOS acceleration) from steps with the hand.
            // A fixed 6 pt per count let a one-point carry error through at 30x: 30 screen pt on 6
            // counts, a jump the hand was moving 0.7 pt per count for (explorer 2026-10-30).
            let c = abs(a.x) + abs(a.y)
            if c >= 2 {
                let seen = (abs(dx) + abs(dy)) * lv / c
                ptsPerCount += (min(seen, 2 * ptsPerCount + 1) - ptsPerCount) * 0.15
            }
        }
        if abs(dx) + abs(dy) + abs(withHand.x) + abs(withHand.y) < 0.001 || lv <= 1.0001 { return p }
        let g = handGain(level: lv)
        if g <= 1.0001 { return p }
        // An exact target collects the hand motion; the pointer can only be placed to whole points
        // (the zoom floors its centre), so the remainder stays in the target instead of being
        // over- or under-shot and corrected next frame (that back-and-forth shook the view,
        // most while zooming: measured 2026-10-30).
        var t = handTarget ?? prev
        t = Pt(min(max(t.x + dx * g + withHand.x, 0), screen.w - 1), min(max(t.y + dy * g + withHand.y, 0), screen.h - 1))
        handTarget = t
        // The carry ends the view where the frame's view will be (see the view target in tick): on
        // the hand's path while the level changes, else where the carry lands the pointer, so no
        // later write in the frame moves the floored centre.
        // While the level changes, the pointer is placed toward an evened-out (alpha-beta smoothed)
        // target, so its whole-point carries come out even and the view stays on the pointer
        // (locked) without stepping unevenly. Measured on diagonal pans while zooming to 15-20x:
        // pointer movement on screen 6.6-7.6 -> 4.5-5.2 px a frame (a steady-zoom pan: 5.4-6.2).
        var aim = t
        if zoom.isMoving || abs(lv - liveLevel) > 1e-6 {
            let pr = Pt(tSmooth.x + tVel.x * dt, tSmooth.y + tVel.y * dt)
            let rx = t.x - pr.x, ry = t.y - pr.y
            tSmooth = Pt(pr.x + 0.5 * rx, pr.y + 0.5 * ry)
            tVel = (tVel.x + 0.15 * rx / max(dt, 0.001), tVel.y + 0.15 * ry / max(dt, 0.001))
            // never past the hand's own target, never behind the pointer, and at most 1.5 pt behind
            // the target: starting from rest the smoothing trailed a fast hand by 9 pt and then
            // carried it all in one frame (explorer, circles while zooming in, 2026-10-31)
            func clampAxis(_ a: Double, _ p: Double, _ t: Double) -> Double {
                let v = t >= p ? min(max(a, p), t) : max(min(a, p), t)
                return t >= p ? max(v, t - 1.5) : min(v, t + 1.5)
            }
            aim = Pt(clampAxis(tSmooth.x, p.x, t.x), clampAxis(tSmooth.y, p.y, t.y))
        } else { tSmooth = t; tVel = (0, 0) }
        // Carry by whole DRAWN points: the zoom shows floor(pointer), so in the hand's direction the
        // pointer is placed on the aim's whole point, not a whole-point distance toward it. Toward-zero
        // left it up to a point behind; native motion and a carry then crossed two points in one frame
        // (slow pan at 4x: drawn view steps 0,2,0,1 instead of 1,1,1; measured 2026-10-31). Against
        // the hand it never steps (the pointer just ahead of the aim stays until the aim catches up).
        func whole(_ a: Double, _ p: Double, _ d: Double) -> Double {
            let kt = (a - p).rounded(.towardZero), kf = floor(a) - floor(p)
            // Only from half a point off: a hair past a point boundary (turning in circles) carried
            // a whole point for almost no hand motion (explorer: 1.3 pt snaps at 7-16x).
            let k = d == 0 || kf * d < 0 || abs(a - p) < 0.5 ? kt : kf
            return k == 0 ? p : p + k + (k > 0 ? 0.5 : -0.5)    // places exactly k (toward zero)
        }
        // With the view pinned to an edge the cursor moves across the screen, and there whole-point
        // carries showed as hops of a level's pixels every few frames (cursor_truth edgepan/edgehold:
        // 29-66% of frames jerked). A pinned axis is placed on the exact aim by a warp instead
        // (precise to 1/level of a point, see placePointer), so it keeps the aim as it is.
        // Judged on the centre as written this frame (the level is written first): last frame's centre,
        // a fraction inside the new level's edge while zooming in, read as not pinned and the carries
        // went back to whole points (cursor hops of 20-30 px zooming at an edge, 2026-10-31).
        let hwE = screen.w / lv / 2, hhE = screen.h / lv / 2
        let pinX = (liveCenter.x <= hwE + 1e-6 && aim.x <= hwE) || (liveCenter.x >= screen.w - hwE - 1e-6 && aim.x >= screen.w - hwE)
        let pinY = (liveCenter.y <= hhE + 1e-6 && aim.y <= hhE) || (liveCenter.y >= screen.h - hhE - 1e-6 && aim.y >= screen.h - hhE)
        aim = Pt(pinX ? aim.x : whole(aim.x, p.x, dx * g + withHand.x), pinY ? aim.y : whole(aim.y, p.y, dy * g + withHand.y))
        let land = Pt(p.x + (aim.x - p.x).rounded(.towardZero), p.y + (aim.y - p.y).rounded(.towardZero))
        let got = placePointer(at: aim, from: p, level: lv, view: config.mouseAlign == 0 ? mouseTarget(land, lv) : nil)
        if config.logLevel >= 2 {
            Log.verbose(String(format: "comp prev %.2f p %.2f want %.2f got %.2f centre %.2f", prev.x, p.x, t.x, got.x, center.x))
        }
        return got
    }

    /// If the pointer moved back by (about) the whole shift last tick's placements made, macOS
    /// undid them: the position the hand is measured from is the one before the placement.
    private func undonePlacement(_ prev: Pt, _ p: CGPoint) -> Pt? {
        let s = placedLastTick
        func undone(_ d: Double, _ s: Double) -> Bool { abs(s) >= 0.9 && abs(d + s) < 0.35 + 0.02 * abs(s) }
        let ux = undone(p.x - prev.x, s.x), uy = undone(p.y - prev.y, s.y)
        guard ux || uy else { return nil }
        Log.verbose(String(format: "placement undone: shift %.2f %.2f, pointer moved %.2f %.2f", s.x, s.y, p.x - prev.x, p.y - prev.y))
        // macOS can also re-apply the undone carry a frame later: a step matching it is not taken as
        // the hand twice (a 17 pt forward snap leaving the left edge, explorer 2026-10-30).
        if ux { redoX = s.x }
        if uy { redoY = s.y }
        redoAge = 0
        return Pt(ux ? prev.x - s.x : prev.x, uy ? prev.y - s.y : prev.y)
    }

    /// Slow pans at high zoom step a whole point (level screen pixels) at a time, with idle frames
    /// between. Experimental (config slowPanGlide, issue #13): each whole-point step is one
    /// animated keepPointer=false write, so WindowServer glides the view and the pointer through the
    /// fraction over about the time the hand takes per point. During a glide no other write is made
    /// (a plain write would be overwritten by its timer) and the hand is measured from raw counts
    /// (learnt points per count), since pointer reads include the glide.
    private func slowPanGlide(_ ptr: CGPoint, _ lv: Double, _ dt: Double, _ r: (dx: Double, dy: Double)) -> Bool {
        glideFrame = false
        let now = CACurrentMediaTime()
        let running = now < glideUntil
        // Hand speed (1x points/s) as measured everywhere else (handSpeedAvg); hysteresis so the mode
        // does not flap around the threshold (each switch cost motion).
        // A fast hand shows in the raw counts at once (the glide reads come only every ~50 ms).
        // Smoothed over ~50 ms: a single count landing in one 7 ms frame is not a fast hand.
        let rk = exp(-dt / 0.05)
        rawSpeedAvg = rawSpeedAvg * rk + (r.dx * r.dx + r.dy * r.dy).squareRoot() * min(max(ptsPerCount, 0.5), 1.5) / max(dt, 0.001) * (1 - rk)
        let speed = max(handSpeedAvg, rawSpeedAvg > 150 ? rawSpeedAvg : 0)
        let hw = screen.w / lv / 2, hh = screen.h / lv / 2
        let free = liveCenter.x > hw + 3 && liveCenter.x < screen.w - hw - 3 && liveCenter.y > hh + 3 && liveCenter.y < screen.h - hh - 3
        let want = config.slowPanGlide && lv >= 2.5 && abs(lv - liveLevel) < 1e-6 && !zoom.isMoving
            && free && speed < (glideMode ? 80 : 50) && now >= glideCooldown
        guard want || running else {
            // Back to carries, measured from where the last glide put the pointer: the hand's motion
            // during it is still to be counted (re-reading here dropped it).
            if glideMode { glideMode = false; lastPtr = glideEnd }
            return false
        }
        if !glideMode { glideEnd = lastPtr ?? Pt(ptr.x, ptr.y) }        // the model starts at the pointer
        glideMode = true; glideFrame = true
        if running { virtualPtr = glideEnd; return true }
        // A finished glide lands the pointer exactly on its end plus the hand's motion meanwhile (in
        // the zoomed scale): that difference is the hand, taken at the level gain as in a carry.
        let p = Pt(ptr.x, ptr.y)
        var t = handTarget ?? glideEnd
        if config.logLevel >= 2 { Log.verbose(String(format: "gl read p %.3f end %.3f d %.3f t %.3f", p.x, glideEnd.x, p.x - glideEnd.x, t.x)) }
        let g0 = handGain(level: lv)
        t = Pt(min(max(t.x + (p.x - glideEnd.x) * g0, 0), screen.w - 1), min(max(t.y + (p.y - glideEnd.y) * g0, 0), screen.h - 1))
        handTarget = t
        let span = max(now - lastGlideRead, dt)
        let kk = exp(-span / 0.08)                                             // weighted by the real time
        let moved = ((p.x - glideEnd.x) * (p.x - glideEnd.x) + (p.y - glideEnd.y) * (p.y - glideEnd.y)).squareRoot() * g0
        handSpeedAvg = handSpeedAvg * kk + moved / span * (1 - kk)
        lastGlideRead = now
        // The hand outran the glides (45 ms each, at most 2 points): back to carries for a while.
        // This frame's motion is already in the target, so carries continue from the pointer as read
        // (re-entering at once made the mode flap and the target ran away: 22 of 69 points).
        if abs(t.x - p.x) > 4 || abs(t.y - p.y) > 4 {
            glideMode = false; glideFrame = false; lastPtr = p; glideCooldown = now + 0.4; return false
        }
        // At most two points per glide (a 17-point glide outlasted its measured end); the rest stays
        // in the target for the next one.
        let kx = max(-2, min(2, (t.x - p.x).rounded(.towardZero))), ky = max(-2, min(2, (t.y - p.y).rounded(.towardZero)))
        if kx == 0 && ky == 0 { virtualPtr = p; glideEnd = p; return true }
        let c = Pt(floor(liveCenter.x), floor(liveCenter.y))
        let a = Pt(c.x + 0.5, c.y + 0.5), b = Pt(c.x + kx + 0.5, c.y + ky + 0.5)
        if abs(a.x - liveCenter.x) > 1e-9 || abs(a.y - liveCenter.y) > 1e-9 {
            SkyLight.set(center: CGPoint(x: a.x, y: a.y), scale: lv, smooth: config.smoothImages, keepPointer: true)
        }
        // WindowServer picks the duration from a few coarse values: measured ~45 ms (its 0.041 s
        // value plus the 10 ms timer) at 3x-30x for 1-2 point steps, whatever shorter one is asked.
        let T = 0.041
        let dist = (kx * kx + ky * ky).squareRoot()
        let g = min(0.5, max(0.0005, Double.pi * (lv * dist).squareRoot() / (1500 * T)))
        SkyLight.set(center: CGPoint(x: b.x, y: b.y), scale: lv, smooth: config.smoothImages, keepPointer: false, glide: g)
        liveCenter = b; liveLevel = lv; lastWritten = (b, lv); wroteThisTick = true
        glideUntil = now + 0.052                                            // measured end plus a margin
        glideEnd = Pt(p.x + kx, p.y + ky)
        virtualPtr = glideEnd
        if config.logLevel >= 2 { Log.verbose(String(format: "glide k %.0f,%.0f speed %.1f pt/s g %.3f", kx, ky, speed, g)) }
        return true
    }

    /// How much a zoomed pointer step is scaled to cover the 1x desktop distance.
    private func handGain(level lv: Double) -> Double {
        guard config.slowPrecision else { return lv }
        // Very slow hand motion keeps macOS's fine zoomed steps (no jump-and-pause, no lag);
        // from a normal speed on, the full 1x desktop speed applies, like pointer acceleration.
        // Hand speed (pt/s at 1x) is averaged over ~80 ms: slow motion arrives as one step then
        // idle frames, so a single frame read twice the real speed (measured 2026-10-30).
        let ramp = min(max((handSpeedAvg - config.precisionBelow) / config.precisionBelow, 0), 1)
        return 1 + (lv - 1) * ramp
    }

    /// The pan glide (panSmoothMs). At 1x speed, slow hand motion arrives as single mouse steps of
    /// 4.7 screen pt at 4x (10.7 at 10x) with idle frames between: the view jumped and paused,
    /// which read as shaking (owner report 2026-10-30). The POINTER glides, not the view: an exact
    /// target collects the hand motion (x level, so the 1x distance is kept), the pointer is placed
    /// on a short spring toward it, and the view stays centred on the pointer. Gliding the view
    /// instead left the pointer off-centre, where its readback picks up rounding jumps that the
    /// compensation multiplied by the level (speed varied +-30%, measured).
    private func glidePointer(from prev: Pt, now p: Pt, level lv: Double, dt: Double) -> Pt {
        let g = handGain(level: lv)
        var t = handTarget ?? prev
        t = Pt(min(max(t.x + (p.x - prev.x) * g, 0), screen.w - 1), min(max(t.y + (p.y - prev.y) * g, 0), screen.h - 1))
        handTarget = t
        var vx = ptrVel.x, vy = ptrVel.y
        let x = springToward(prev.x, t.x, vel: &vx, dtMs: dt * 1000, glideMs: config.panSmoothMs)
        let y = springToward(prev.y, t.y, vel: &vy, dtMs: dt * 1000, glideMs: config.panSmoothMs)
        ptrVel = (vx, vy)
        if abs(x - p.x) + abs(y - p.y) < 0.01 { return p }
        return placePointer(at: Pt(x, y), from: p, level: lv, view: config.mouseAlign == 0 ? mouseTarget(Pt(x, y), lv) : nil)
    }

    /// Move the pointer to a desktop point while zoomed. A warp is reinterpreted in screen space by
    /// the zoom, but a view write that keeps the pointer's SCREEN position (keepPointer=false)
    /// carries it across the desktop by exactly the view's shift. As two writes:
    /// 1. keepPointer=true to `a` (the pointer keeps its desktop position), skipped when the view
    ///    is already there;
    /// 2. keepPointer=false to `b`, the whole-point centre the view should end on (`view`; without
    ///    one, the current view shifted by the carry). The pointer moves b - a.
    /// So the carry is the frame's last view change: a keepPointer=true write that moved the
    /// floored centre after it left the next read off (2026-10-30). Near a screen edge `a` sits
    /// inward of `b`, which is how the pointer still reaches the edge (2026-10-29: without it the
    /// pointer fell back to macOS's slow speed there). Writes use the scale the compositor shows
    /// (`liveLevel`). `p` is the tick's own read: a read here, after the level write, can be off.
    /// Returns where the pointer is now (p plus the whole points carried), computed, not read.
    /// A warp while zoomed sets the cursor's SCREEN position: from the next hand motion on, the
    /// pointer reads view origin + target / level (measured 2026-10-31, every edge, 5.25x and 8x),
    /// but until that motion a read returns the target itself. `pointer()` corrects such a read.
    nonisolated(unsafe) static var pendingWarp: (s: CGPoint, d: CGPoint)?

    /// The pointer's desktop position (top-left origin), corrected for a warp not yet read back.
    static func pointer() -> CGPoint? {
        guard let q = CGEvent(source: nil)?.location else { return nil }
        if let w = pendingWarp {
            if abs(q.x - w.s.x) < 1e-3 && abs(q.y - w.s.y) < 1e-3 { return w.d }
            pendingWarp = nil
        }
        return q
    }

    private static let noSuppression = CGEventSource(stateID: .combinedSessionState)

    private func placePointer(at t: Pt, from p: Pt, level lv: Double, view: Pt? = nil) -> Pt {
        if DrawZoom.enabled {                           // macOS is not zoomed: a warp is exact
            let w = CGPoint(x: min(max(t.x, 0), screen.w - 1), y: min(max(t.y, 0), screen.h - 1))
            // Without this a warp holds the hand's motion back for a moment (CGAssociate... did not
            // lift it): leaving Inspect while moving, the pointer stood still ~0.1 s while the view
            // followed the hand, then caught up, dragging the view (owner 2026-11-01).
            Self.noSuppression?.localEventsSuppressionInterval = 0
            CGWarpMouseCursorPosition(w); CGAssociateMouseAndMouseCursorPosition(1)
            return Pt(w.x, w.y)
        }
        // The zoom floors its centre to whole desktop points (measured 2026-10-30), so a view shift
        // can only carry the pointer by whole points. Plan in the compositor's integer centre and
        // pass centres at .5 so the floor lands exactly. Shift only whole points TOWARD the target,
        // never past it: rounding to nearest overshot by up to half a point, a bit of hand motion
        // in between made it more, and the next frame stepped back (one-frame back-steps while
        // zooming and panning). The remainder stays in the caller's exact target.
        // The pointer can read size (1920.000) at the far edges while targets stop at size - 1:
        // measured from the raw read, a placement pulled it a point inward with the hand still.
        let want = Pt(min(max(t.x, 0), screen.w - 1), min(max(t.y, 0), screen.h - 1))
        let px = min(max(p.x, 0), screen.w - 1), py = min(max(p.y, 0), screen.h - 1)
        let kx = (want.x - px).rounded(.towardZero), ky = (want.y - py).rounded(.towardZero)
        let live = max(1, liveLevel)
        let halfW = screen.w / live / 2, halfH = screen.h / live / 2
        // The view pinned to an edge on a carried axis, and staying pinned: a carry by view writes must
        // move the view off the edge and back within the frame (183.5, 184.5, then 181.8 at 5.25x,
        // where the edge is 182.86); when those writes reached different frames the pinned view
        // jumped out and back (owner 2026-10-31: "the window wants to move but isn't allowed";
        // scripts/edge_truth.py sees it on the zoomed screen). There the pointer is warped instead,
        // to the SCREEN position of the target (a zoomed warp sets the cursor's screen position, in
        // whole screen points): the view never moves, and the carry lands within 1/level of a point.
        func pinned(_ live: Double, _ v: Double?, _ half: Double, _ size: Double) -> Bool {
            guard let v else { return false }
            return (live <= half + 1e-6 && v <= half + 1e-6) || (live >= size - half - 1e-6 && v >= size - half - 1e-6)
        }
        // A pinned axis goes to the exact target (the cursor moves across the screen there, and whole
        // points hopped by a level's pixels); a free axis by whole points as below.
        let pinX = pinned(liveCenter.x, view?.x, halfW, screen.w), pinY = pinned(liveCenter.y, view?.y, halfH, screen.h)
        let ex = pinX && abs(want.x - px) >= 0.5 / live ? want.x - px : 0
        let ey = pinY && abs(want.y - py) >= 0.5 / live ? want.y - py : 0
        let mx = pinX ? ex : kx, my = pinY ? ey : ky
        if mx == 0 && my == 0 && kx == 0 && ky == 0 { return p }
        // The view's top-left corner as shown: the whole written centre, clamped by the compositor.
        let ox = min(max(floor(liveCenter.x), halfW), screen.w - halfW) - halfW
        let oy = min(max(floor(liveCenter.y), halfH), screen.h - halfH) - halfH
        // Whole carries as planned (the remainder stays in the caller's target), on an exact base.
        // A warp is absolute: hand motion that arrived since this tick's read would be lost (a small
        // step back, explorer 2026-10-31), so it is added from a fresh read (no zoom write since).
        let late = Magnifier.pointer().map { Pt($0.x - p.x, $0.y - p.y) } ?? Pt(0, 0)
        let lateOK = abs(late.x) < 3 && abs(late.y) < 3
        let d = Pt(min(max(px + mx + (lateOK ? late.x : 0), 0), screen.w - 1), min(max(py + my + (lateOK ? late.y : 0), 0), screen.h - 1))
        let s = CGPoint(x: ((d.x - ox) * live).rounded(), y: ((d.y - oy) * live).rounded())
        // Only to a point the view shows: a warp reaches screen positions only (Inspect pans the view
        // away from its frozen pointer, and a warp there threw the pointer to x = 0).
        let onScreen = s.x >= 0 && s.y >= 0 && s.x <= screen.w - 1 && s.y <= screen.h - 1
        if onScreen && ((pinX && mx != 0) || (pinY && my != 0)) {
            let got = CGPoint(x: ox + s.x / live, y: oy + s.y / live)
            Self.noSuppression?.localEventsSuppressionInterval = 0    // else the hand is held back 250 ms
            CGWarpMouseCursorPosition(s)
            Self.pendingWarp = (s, got)
            if config.logLevel >= 3 { Log.verbose(String(format: "warp carry %.4f %.4f -> screen %.0f %.0f = %.4f %.4f", p.x, p.y, s.x, s.y, got.x, got.y)) }
            placedThisTick = Pt(placedThisTick.x + got.x - p.x - (lateOK ? late.x : 0), placedThisTick.y + got.y - p.y - (lateOK ? late.y : 0))
            return Pt(got.x - (lateOK ? late.x : 0), got.y - (lateOK ? late.y : 0))
        }
        if kx == 0 && ky == 0 { return p }
        // Per axis: the centres (as written) before and after the carry, and the carry. An axis
        // that does not move is left as it is: rewriting it at an edge, where the zoom clamps the
        // view to a fractional centre, moved the pointer and pinned it against the edge (a "wall"
        // along the Dock, owner report 2026-10-30).
        func axis(_ live: Double, _ k: Double, _ v: Double?, _ half: Double, _ size: Double) -> (Double, Double, Double) {
            let lo = ceil(half), hi = floor(size - half)
            if k == 0 || lo > hi { return (live, live, 0) }
            let c = min(max(floor(live), lo), hi)                    // the whole centre shown now
            func plan(_ end: Double) -> (Double, Double, Double) {
                let b = min(max(end, lo), hi)                        // where the view ends
                let a = min(max(b - k, lo), hi)                      // the carry must fit inside
                return (a + 0.5, b + 0.5, b - a)
            }
            let shift = plan(c + k)                                  // the view shifted by the carry
            guard let v else { return shift }
            let follow = plan(floor(v))
            // Ending on `view` cannot carry the whole way where it sits on a screen edge; the
            // shifted view (recentred by the tick's normal write) still can.
            return abs(follow.2) >= abs(shift.2) ? follow : shift
        }
        let (ax, bx, cx) = axis(liveCenter.x, kx, view?.x, halfW, screen.w)
        let (ay, by, cy) = axis(liveCenter.y, ky, view?.y, halfH, screen.h)
        if cx == 0 && cy == 0 { return p }
        if ax != liveCenter.x || ay != liveCenter.y {
            SkyLight.set(center: CGPoint(x: ax, y: ay), scale: live, smooth: config.smoothImages, keepPointer: true, drawnFollow: false)
        }
        SkyLight.set(center: CGPoint(x: bx, y: by), scale: live, smooth: config.smoothImages, keepPointer: false)
        liveCenter = Pt(bx, by)
        liveLevel = live
        lastWritten = (Pt(bx, by), live)
        carryView = Pt(bx, by)
        wroteThisTick = true
        placedThisTick = Pt(placedThisTick.x + cx, placedThisTick.y + cy)
        return Pt(p.x + cx, p.y + cy)
    }

    private func mouseTarget(_ p: Pt, _ lv: Double) -> Pt {
        if config.mouseAlign == 1 {
            return edgePanCenter(cur: center, ptr: p, level: lv, screen: screen,
                                 marginPct: config.mouseMarginPct, body: .arrow)
        }
        return clampCenter(p, level: lv, screen: screen)
    }

    private func write(_ lv: Double) {
        if let w = lastWritten, abs(w.c.x - center.x) < 0.01, abs(w.c.y - center.y) < 0.01, abs(w.level - lv) < 1e-5 { return }
        // Same level and the same whole-point centre (away from the edge clamps): the zoom would show
        // exactly the same picture. Skipping the write saves a composite; while panning and zooming
        // at once, extra writes tripled the dropped frames (9% against 3% zooming alone).
        if !DrawZoom.enabled, let w = lastWritten, abs(w.level - lv) < 1e-5, floor(w.c.x) == floor(center.x), floor(w.c.y) == floor(center.y) {
            let hw = screen.w / lv / 2, hh = screen.h / lv / 2
            if center.x > hw + 1 && center.x < screen.w - hw - 1 && center.y > hh + 1 && center.y < screen.h - hh - 1 { return }
        }
        send(center, level: lv, keepPointer: true)
        lastWritten = (center, lv)
    }

    /// One view write. `liveCenter` keeps the value as written (the compositor floors it).
    private func send(_ c: Pt, level lv: Double, keepPointer: Bool) {
        let l = max(1, lv)
        let w = DrawZoom.enabled ? c : Pt(writeAxis(c.x, screen.w / l / 2, screen.w), writeAxis(c.y, screen.h / l / 2, screen.h))
        SkyLight.set(center: CGPoint(x: w.x, y: w.y), scale: lv, smooth: config.smoothImages, keepPointer: keepPointer)
        liveCenter = w
        liveLevel = lv
        wroteThisTick = true
    }

    /// The centre to write for a view centre Wind wants, on one axis. The compositor floors the
    /// centre, then clamps the view to the screen. Held at the far (right or bottom) edge, the clamp
    /// centre size - half is fractional and its floor showed the view up to a point short of the
    /// edge: a pointer on the edge sat outside it and macOS pushed it in (x read 1920.000 / 1919.15
    /// in turn; the explorer, 2026-10-30: ~150 shake reversals at right and bottom edges, ~11 at
    /// left and top). Written a point past it, the compositor's own clamp lands exactly on the edge.
    /// The near edges likewise (see below).
    private func writeAxis(_ c0: Double, _ half: Double, _ size: Double) -> Double {
        // A centre within a hair of a whole point is that point (the zoom floors it): float noise from
        // a screen-keeping write (a pointer at 500.0000 read back as 499.9991) dropped the view a whole
        // point for a frame, and the drawn cursor, placed from the same value, must agree (2026-10-31).
        let c = abs(c0 - c0.rounded()) < 0.002 ? c0.rounded() : c0
        // Both edges: a centre at (or past) the clamp is written a point past it, so the compositor's
        // own clamp lands exactly on the edge. A centre even a fraction inside it pushed a pointer
        // on the near edge inward at every level step while zooming (0 -> 2.5 pt, probe 2026-10-30).
        let hi = size - half
        if c >= hi - 1e-6 && floor(hi) < hi { return hi + 1 }
        if c <= half + 1e-6 { return half - 1 }
        return c
    }

    private func reportLevel() {
        // Render engine: the menu-bar readout waits for the zoom to stop. Retitling the variable-length
        // status item (first "" -> " 1.1x" at lv 1.05) stalled the tick 11-31 ms at every 0.1x crossing
        // while zooming in (owner's log 2026-10-31): a repeated frame, then a double step.
        // (Not at 1x or at the maximum: a held key there leaves the readout stale until release.)
        if DrawZoom.enabled && zoom.isMoving && zoom.level > 1.0001 && zoom.level < config.maxLevel - 1e-6 { return }
        let lv = (zoom.level * 10).rounded() / 10
        if lv != lastReportedLevel { lastReportedLevel = lv; onLevelChange?(lv) }
    }

    // MARK: test hooks

    /// Jump straight to a level (command channel and menu).
    func setLevel(_ lv: Double, glide: Bool) {
        if glide { zoom.glide(to: lv) } else { zoom.setLevel(lv) }
        wake()
    }

    /// Something else changed the shared zoom (macOS Zoom released): write the view again.
    func reassert() {
        lastWritten = nil
        wake()
    }

    func forceLock(_ on: Bool) {
        lock.force(on)
        if !on { lock.reset() }
        wake()
    }

    func setHeld(_ a: Action, _ down: Bool) {
        if down { simHeld.insert(a) } else { simHeld.remove(a) }
        wake()
    }

    private func held(_ a: Action) -> Bool { router.isHeld(a) || simHeld.contains(a) }
}

enum ColorFilter {
    private static var active = false

    /// Warmth lowers blue (and some green) at the top of the transfer curve; dim scales all three.
    static func apply(warmthPct: Double, dimPct: Double) {
        let w = warmthPct / 100, d = dimPct / 100
        if w <= 0 && d >= 1 {
            if active { restore() }
            return
        }
        let r = Float(d), g = Float(d * (1 - 0.22 * w)), b = Float(d * (1 - 0.62 * w))
        var count: UInt32 = 0
        CGGetOnlineDisplayList(0, nil, &count)
        var ids = [CGDirectDisplayID](repeating: 0, count: Int(count))
        CGGetOnlineDisplayList(count, &ids, &count)
        for id in ids {
            let err = CGSetDisplayTransferByFormula(id, 0, r, 1, 0, g, 1, 0, b, 1)
            if err != .success { Log.error("colour filter failed on display \(id): \(err.rawValue)") }
        }
        active = true
        Log.info("colour filter warmth \(Int(warmthPct))% dim \(Int(dimPct))%")
    }

    static func restore() {
        CGDisplayRestoreColorSyncSettings()
        active = false
    }
}
