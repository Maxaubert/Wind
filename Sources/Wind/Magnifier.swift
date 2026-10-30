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
    private var placeSlip = Pt(0, 0)            // hand motion that landed during the last placement
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
        if pointerHiddenForCaret { pointerHiddenForCaret = false; SkyLight.setCursorHidden(false) }
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
        if !pointerHiddenForCaret { SkyLight.setCursorHidden(false) }
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
        let lv = zoom.level

        // A new level is written first, at the view as it stands, before the pointer is read.
        // Written after a placement, a level write sometimes undid it (the pointer snapped back
        // the whole placed shift), and WindowServer applies queued hand motion during it; written
        // here, both land before the read and the frame's placement runs at a steady level, as at
        // a fixed zoom (measured 2026-10-30: the pan slowed 23% and stepped back while zooming).
        if zoomedSession && lv > 1.0001 && abs(lv - liveLevel) > 1e-6 && owner.owner == .mouse && !inspect && !lock.locked {
            let c = clampCenter(liveCenter, level: lv, screen: screen)
            SkyLight.set(center: CGPoint(x: c.x, y: c.y), scale: lv, smooth: config.smoothImages)
            liveCenter = c; liveLevel = lv
            lastWritten = (c, lv)
        }

        let ptr = CGEvent(source: nil)?.location ?? CGPoint(x: center.x, y: center.y)
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
        // A game holding the pointer: the cursor stays hidden while the hand keeps moving. While it
        // holds it, pan from raw HID counts: Wind's own view writes drag the held pointer around and
        // WindowServer reports that as moves with large unrelated deltas, so tap deltas run away.
        let rawOn = (config.lockTracking || inspect) && raw.start()
        let r = raw.take()
        let motion = rawOn ? abs(r.dx) + abs(r.dy) : abs(ldx) + abs(ldy)
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
        if !inspect { lock.feed(cursorVisible: SkyLight.cursorVisible, motion: motion, pointerMoved: moved) }  // Inspect hides it itself
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
            if !inspect { SkyLight.setCursorHidden(hideForCaret) }
        }
        // Text views (TextEdit) show the cursor again as they handle keys: hide it again.
        if pointerHiddenForCaret && !inspect && SkyLight.cursorVisible { SkyLight.setCursorHidden(true, force: true) }

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
                if abs(ptr.x - landed.x) + abs(ptr.y - landed.y) > 0.5 { inspectRef = placePointer(at: a, level: lv) }
            } else {
                let ref = inspectRef ?? a
                if abs(ptr.x - ref.x) + abs(ptr.y - ref.y) > 0.001 {
                    inspectLook = Pt(inspectLook.x + (ptr.x - ref.x) * lv * sens, inspectLook.y + (ptr.y - ref.y) * lv * sens)
                    inspectRef = placePointer(at: a, level: lv)
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
                    virtualPtr = placePointer(at: p, level: lv)
                    handTarget = nil
                } else if pointerToView {
                    pointerToView = false
                    returning = false
                    virtualPtr = placePointer(at: center, level: lv)
                    handTarget = nil
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
                let want = mouseTarget(virtualPtr, lv)
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
        if config.logLevel >= 2 { Log.verbose(String(format: "view %.3f %.3f ptr %.3f %.3f lv %.2f", center.x, center.y, ptr.x, ptr.y, lv)) }
        write(lv)
        crosshair.show(at: inspect ? CGPoint(x: inspectLook.x, y: inspectLook.y) : nil, level: lv)
        var endPtr = (CGEvent(source: nil)?.location).map { Pt($0.x, $0.y) }
        // While the zoom level changes, WindowServer applies queued hand motion during the level
        // write, so the pointer has moved by the time the write returns. That is hand motion in
        // the zoomed (1/level) scale like any other: the target takes it at the same gain, or the
        // pan slowed down while zooming and the next frame pulled the pointer back (measured
        // 2026-10-30: with the hand still, a level write moves the pointer by 0.000 pt).
        if let e = endPtr, let t = handTarget, owner.owner == .mouse, !locked, !inspect {
            let g = handGain(level: lv)
            let nt = Pt(min(max(t.x + (e.x - virtualPtr.x) * g, 0), screen.w - 1),
                        min(max(t.y + (e.y - virtualPtr.y) * g, 0), screen.h - 1))
            handTarget = nt
            // A whole point or more arrived during the write: place it now, not next frame (the
            // view stalled one frame, then jumped twice as far).
            if !returning && (abs(nt.x - e.x) >= 1 || abs(nt.y - e.y) >= 1) {
                virtualPtr = placePointer(at: nt, level: lv)
                takeSlip(gain: g)
                center = clampCenter(mouseTarget(virtualPtr, lv), level: lv, screen: screen)
                write(lv)
                endPtr = (CGEvent(source: nil)?.location).map { Pt($0.x, $0.y) }
                if let e2 = endPtr, let t2 = handTarget {
                    handTarget = Pt(min(max(t2.x + (e2.x - virtualPtr.x) * g, 0), screen.w - 1),
                                    min(max(t2.y + (e2.y - virtualPtr.y) * g, 0), screen.h - 1))
                }
                if config.logLevel >= 2 { Log.verbose(String(format: "view %.3f %.3f ptr %.3f %.3f lv %.2f p2", center.x, center.y, virtualPtr.x, virtualPtr.y, lv)) }
            }
        }
        prevShift = lastShift
        lastShift = Pt(center.x - tickStartCenter.x, center.y - tickStartCenter.y)
        lastPtr = (owner.owner == .mouse && !locked && !inspect) ? endPtr : nil
        afterWrites = endPtr
        reportLevel()

        // Sleep while nothing changes; mouse moves, binds, the wheel and caret events wake it.
        let busy = wroteThisTick || moving || returning || oi.panning || abs(springVel.x) + abs(springVel.y) > 0.01
            || held(.zoomIn) || held(.zoomOut)
        idleTicks = busy ? 0 : idleTicks + 1
        if idleTicks > 30 { idleTicks = 0; l.isPaused = true }
    }

    private func clearSessionFlags() {
        handTarget = nil; ptrVel = (0, 0)
        if pointerHiddenForCaret { pointerHiddenForCaret = false; if !inspect { SkyLight.setCursorHidden(false) } }
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
        let dx = p.x - prev.x, dy = p.y - prev.y
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
        let got = placePointer(at: t, level: lv)
        takeSlip(gain: g)
        if config.logLevel >= 2 {
            Log.verbose(String(format: "comp prev %.2f p %.2f want %.2f got %.2f centre %.2f", prev.x, p.x, t.x, got.x, center.x))
        }
        return got
    }

    /// Hand motion that arrived during a placement moved the pointer at the zoomed (1/level)
    /// scale; the target takes it at the hand gain like any other motion.
    private func takeSlip(gain g: Double) {
        guard let t = handTarget, placeSlip.x != 0 || placeSlip.y != 0 else { return }
        handTarget = Pt(min(max(t.x + placeSlip.x * g, 0), screen.w - 1),
                        min(max(t.y + placeSlip.y * g, 0), screen.h - 1))
        placeSlip = Pt(0, 0)
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
        return placePointer(at: Pt(x, y), level: lv)
    }

    /// Move the pointer to a desktop point while zoomed. A warp is reinterpreted in screen space by
    /// the zoom, but a view write that keeps the pointer's SCREEN position carries it across the
    /// desktop by exactly the view's shift; the next normal write recentres.
    /// Near a screen edge the view cannot shift outward (the zoom clamps it). Then the shift is
    /// done the other way round: first move the view inward keeping the pointer's DESKTOP position,
    /// then move it back keeping its SCREEN position. The view ends where it was and the pointer has
    /// moved outward by the same amount. Writes use the scale the compositor shows (`liveLevel`):
    /// a new level here would rescale the pointer's screen offset; the normal write applies it.
    private func placePointer(at t: Pt, level lv: Double) -> Pt {
        let want = Pt(min(max(t.x, 0), screen.w - 1), min(max(t.y, 0), screen.h - 1))
        let p = CGEvent(source: nil)?.location ?? CGPoint(x: want.x, y: want.y)
        // The zoom floors its centre to whole desktop points (measured 2026-10-30), so a view shift
        // can only carry the pointer by whole points. Plan in the compositor's integer centre and
        // pass centres at .5 so the floor lands exactly. Shift only whole points TOWARD the target,
        // never past it: rounding to nearest overshot by up to half a point, a bit of hand motion
        // in between made it more, and the next frame stepped back (one-frame back-steps while
        // zooming and panning). The remainder stays in the caller's exact target.
        let kx = (want.x - p.x).rounded(.towardZero), ky = (want.y - p.y).rounded(.towardZero)
        placeSlip = Pt(0, 0)
        if kx == 0 && ky == 0 { return Pt(p.x, p.y) }
        let live = max(1, liveLevel)
        let halfW = screen.w / live / 2, halfH = screen.h / live / 2
        let a = Pt(floor(liveCenter.x), floor(liveCenter.y))       // the centre the compositor shows
        // Per axis: (start, end) whole-point centres of the screen-position-keeping shift.
        func axis(_ c: Double, _ k: Double, _ half: Double, _ size: Double) -> (Double, Double) {
            let lo = ceil(half), hi = floor(size - half)
            if c + k >= lo && c + k <= hi { return (c, c + k) }        // room to shift: one write
            let k2 = max(min(k, c - lo), c - hi)                        // the shift must fit inward
            return (c - k2, c)
        }
        let (ax, bx) = axis(a.x, kx, halfW, screen.w)
        let (ay, by) = axis(a.y, ky, halfH, screen.h)
        if ax != a.x || ay != a.y {
            SkyLight.set(center: CGPoint(x: ax + 0.5, y: ay + 0.5), scale: live, smooth: config.smoothImages, keepPointer: true)
        }
        SkyLight.set(center: CGPoint(x: bx + 0.5, y: by + 0.5), scale: live, smooth: config.smoothImages, keepPointer: false)
        liveCenter = Pt(bx + 0.5, by + 0.5)
        lastWritten = nil
        wroteThisTick = true
        let got = CGEvent(source: nil)?.location ?? CGPoint(x: want.x, y: want.y)
        // The shift carries the pointer exactly bx - ax; anything more is hand motion WindowServer
        // applied during the writes (most while the level changes).
        placeSlip = Pt(got.x - (p.x + bx - ax), got.y - (p.y + by - ay))
        return Pt(got.x, got.y)
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
        SkyLight.set(center: CGPoint(x: center.x, y: center.y), scale: lv, smooth: config.smoothImages)
        lastWritten = (center, lv)
        liveCenter = center
        liveLevel = lv
        wroteThisTick = true
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
