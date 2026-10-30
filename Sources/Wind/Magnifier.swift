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
    private var prevRawMotion = 0.0
    private var placedThisTick = Pt(0, 0)       // whole points Wind's carries moved the pointer this tick
    private var placedLastTick = Pt(0, 0)
    private var carryView: Pt?                  // the centre this tick's carry ended the view on
    private var pendingJump: Pt?                // a far pointer read waiting for a second one
    private var rawStep: Pt?                    // raw counts this tick (nil when none flow)
    private var prevRawStep: Pt?
    private var pushX = 0.0, pushY = 0.0       // rejected non-hand pointer pushes awaiting their return
    private var redoX = 0.0, redoY = 0.0, redoAge = 0   // an undone carry macOS may re-apply
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
        let p = CGEvent(source: nil)?.location ?? .zero
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

    @objc private func tick(_ l: CADisplayLink) {
        let now = l.timestamp
        let dt = lastTick == 0 ? l.duration : min(0.1, now - lastTick)
        lastTick = now
        if paused { l.isPaused = true; return }

        zoom.direction = resolveDirection(inHeld: held(.zoomIn), outHeld: held(.zoomOut))
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
            send(c, level: lv, keepPointer: true)
            lastWritten = (c, lv)
            levelWritten = true
        }

        // The pointer is read ONCE per tick, here; every later step works from this read and from
        // the whole points Wind's own carries move it, never from a read after a write. Right after
        // a level write the first read can be tens of points off for about 0.5 ms, then holds
        // (PROBE at the bottom edge, 2026-10-30: 989.08 then 1082.88 x5): read until two agree.
        var ptr = CGEvent(source: nil)?.location ?? CGPoint(x: center.x, y: center.y)
        if levelWritten {
            for _ in 0..<3 {
                usleep(500)
                guard let q = CGEvent(source: nil)?.location else { break }
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
           rawOn, CACurrentMediaTime() - lastRawAt < 1 {
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
            let candidates = [Pt(0, 0), lastShift, prevShift, Pt(lastShift.x + prevShift.x, lastShift.y + prevShift.y)]
            return candidates.map { abs(mx - $0.x) + abs(my - $0.y) }.min() ?? 0
        } ?? 0
        // In screen points: at a high zoom a slow hand moves the pointer a tiny fraction of a desktop
        // point per frame, which read as held and flipped a false game lock on and off (it pulled
        // the view into a corner, owner recording 2026-10-30).
        // A pointer pushed against a screen edge stays put while the hand moves, exactly like one a
        // game holds: that is no evidence (every false lock in the owner's recording, 2026-10-30,
        // began with the pointer at x = 0 and macOS reporting the cursor hidden).
        let atEdge = ptr.x < 2 || ptr.y < 2 || ptr.x > screen.w - 3 || ptr.y > screen.h - 3
        if !inspect && !userHidCursor { lock.feed(cursorVisible: SkyLight.cursorVisible, motion: atEdge && !lock.locked ? 0 : motion, pointerMoved: moved * max(lv, 1)) }  // Inspect hides it itself
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
        if wasLocked && !locked { returning = true }
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
                else if prevOwner != .mouse { returning = true }
                virtualPtr = Pt(ptr.x, ptr.y)
                let onZoomedDisplay = ptr.x >= 0 && ptr.y >= 0 && ptr.x < screen.w && ptr.y < screen.h
                if !onZoomedDisplay {
                    pointerToView = false               // never drag the pointer across displays
                    handTarget = nil
                } else if let p = pendingPlace {
                    pendingPlace = nil
                    returning = false
                    virtualPtr = placePointer(at: p, from: virtualPtr, level: lv, view: center)
                    handTarget = nil
                } else if pointerToView {
                    pointerToView = false
                    returning = false
                    virtualPtr = placePointer(at: center, from: virtualPtr, level: lv, view: center)
                    handTarget = nil
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
                    // 1920.000 / 1919.15 every frame while zooming with the hand still).
                    handTarget = nil
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
                var want = mouseTarget(virtualPtr, lv)
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
                    var vx = springVel.x, vy = springVel.y
                    let x = springToward(center.x, want.x, vel: &vx, dtMs: dt * 1000, glideMs: 160)
                    let y = springToward(center.y, want.y, vel: &vy, dtMs: dt * 1000, glideMs: 160)
                    springVel = (vx, vy)
                    target = Pt(x, y)
                    if abs(x - want.x) + abs(y - want.y) < 0.5 { returning = false; springVel = (0, 0) }
                } else {
                    target = want
                    springVel = (0, 0)
                }
            case .caret, .focus:
                if let t = trackTargetCenter(owner.target.rect, cur: center, level: lv, screen: screen,
                                             align: config.trackAlign, marginPct: config.trackMarginPct) {
                    var vx = springVel.x, vy = springVel.y
                    let x = springToward(center.x, t.x, vel: &vx, dtMs: dt * 1000, glideMs: config.trackGlideMs)
                    let y = springToward(center.y, t.y, vel: &vy, dtMs: dt * 1000, glideMs: config.trackGlideMs)
                    springVel = (vx, vy)
                    target = Pt(x, y)
                }
            case .keys:
                let v = keyPanSpeed(level: lv, screenWidth: screen.w, panSpeed: config.panSpeed) * dt
                target = Pt(center.x + panX * v, center.y + panY * v)
            }
        }
        center = clampCenter(target, level: lv, screen: screen)
        if config.logLevel >= 2 { Log.verbose(String(format: "view %.4f %.4f ptr %.4f %.4f lv %.5f", center.x, center.y, ptr.x, ptr.y, lv)) }
        // The crosshair window moves first and its move is committed before the zoom write, so both
        // land in the same frame; moved after, it trailed the view by a frame while panning.
        crosshairAt = inspect ? crosshairPoint(lv, centre: center) : nil
        crosshair.show(at: crosshairAt, level: lv)
        write(lv)
        // A plain read after the writes, for the game-lock detector only (a held pointer is dragged
        // by Wind's writes). It is never used to measure the hand.
        let endPtr = CGEvent(source: nil).map { Pt($0.location.x, $0.location.y) }
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
        if var m = lastPtr, let e = endPtr, rawStep != nil {
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
    private func crosshairPoint(_ lv: Double, centre: Pt) -> CGPoint {
        func axis(_ c: Double, _ look: Double, _ size: Double) -> Double {
            let half = size / lv / 2, shown = floor(writeAxis(c, half, size))
            return shown >= half && shown <= size - half ? shown : look
        }
        return CGPoint(x: axis(centre.x, inspectLook.x, screen.w), y: axis(centre.y, inspectLook.y, screen.h))
    }

    private func clearSessionFlags() {
        handTarget = nil; ptrVel = (0, 0)
        pushX = 0; pushY = 0; redoX = 0; redoY = 0; rawStep = nil; prevRawStep = nil; pendingJump = nil
        if pointerHiddenForCaret { pointerHiddenForCaret = false; if !inspect && !userHidCursor { SkyLight.setCursorHidden(false) } }
        afterWrites = nil
        pointerToView = false
        pendingPlace = nil
        returning = false
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
        // Bounded by the hand: on each axis a step can be no bigger on screen than the raw counts of
        // this and the last tick allow (macOS moves ~0.7 pt per count, up to ~3 with acceleration).
        // Anything more is Wind's own carry landing a point off its model, and multiplied by the
        // level it snapped the view back and forth (explorer 2026-10-30: +-5 pt at 7.3x on 0-4 counts).
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
            func bound(_ d: Double, _ a: Double, _ b: Double, _ pend: inout Double, _ redo: inout Double) -> Double {
                var d = d
                if redo != 0 && d * redo > 0 && abs(d - redo) <= 0.5 + 0.3 * abs(redo) { d -= redo; redo = 0 }
                if d * pend < 0 {                                   // moving back the way it was pushed
                    let take = d > 0 ? min(d, -pend) : max(d, -pend)
                    d -= take; pend += take
                }
                let lo = (6 * min(0, a) + 6 * min(0, b) - 2) / lv, hi = (6 * max(0, a) + 6 * max(0, b) + 2) / lv
                let kept = min(max(d, lo), hi)
                let against = (a + b) == 0 ? 0 : (d - kept) * (a + b) < 0 ? d - kept : 0
                pend += against
                return kept
            }
            dx = bound(dx, a.x, b.x, &pushX, &redoX); dy = bound(dy, a.y, b.y, &pushY, &redoY)
        }
        if abs(dx) + abs(dy) < 0.001 || lv <= 1.0001 { return p }
        let g = handGain(level: lv)
        if g <= 1.0001 { return p }
        // An exact target collects the hand motion; the pointer can only be placed to whole points
        // (the zoom floors its centre), so the remainder stays in the target instead of being
        // over- or under-shot and corrected next frame (that back-and-forth shook the view,
        // most while zooming: measured 2026-10-30).
        var t = handTarget ?? prev
        t = Pt(min(max(t.x + dx * g, 0), screen.w - 1), min(max(t.y + dy * g, 0), screen.h - 1))
        handTarget = t
        // The view ends centred where the carry lands the pointer (whole points toward the target),
        // not on the target: ending on the target's floor left the pointer a point short and the
        // next frame recentred a point back (back-steps while zooming and panning).
        let land = Pt(p.x + (t.x - p.x).rounded(.towardZero), p.y + (t.y - p.y).rounded(.towardZero))
        let got = placePointer(at: t, from: p, level: lv, view: config.mouseAlign == 0 ? mouseTarget(land, lv) : nil)
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
    private func placePointer(at t: Pt, from p: Pt, level lv: Double, view: Pt? = nil) -> Pt {
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
        if kx == 0 && ky == 0 { return p }
        let live = max(1, liveLevel)
        let halfW = screen.w / live / 2, halfH = screen.h / live / 2
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
            SkyLight.set(center: CGPoint(x: ax, y: ay), scale: live, smooth: config.smoothImages, keepPointer: true)
        }
        SkyLight.set(center: CGPoint(x: bx, y: by), scale: live, smooth: config.smoothImages, keepPointer: false)
        liveCenter = Pt(bx, by)
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
        send(center, level: lv, keepPointer: true)
        lastWritten = (center, lv)
    }

    /// One view write. `liveCenter` keeps the value as written (the compositor floors it).
    private func send(_ c: Pt, level lv: Double, keepPointer: Bool) {
        let l = max(1, lv)
        let w = Pt(writeAxis(c.x, screen.w / l / 2, screen.w), writeAxis(c.y, screen.h / l / 2, screen.h))
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
    private func writeAxis(_ c: Double, _ half: Double, _ size: Double) -> Double {
        // Both edges: a centre at (or past) the clamp is written a point past it, so the compositor's
        // own clamp lands exactly on the edge. A centre even a fraction inside it pushed a pointer
        // on the near edge inward at every level step while zooming (0 -> 2.5 pt, probe 2026-10-30).
        let hi = size - half
        if c >= hi - 1e-6 && floor(hi) < hi { return hi + 1 }
        if c <= half + 1e-6 { return half - 1 }
        return c
    }

    private func reportLevel() {
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
