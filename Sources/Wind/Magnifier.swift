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

    private var tap: CFMachPort?
    private var link: CADisplayLink?
    private var lastTick: CFTimeInterval = 0

    // Input state, written by the tap.
    private var mods: Mods = []
    private var buttonsDown = 0
    private var lastButtonAt: CFTimeInterval = -1e9
    private var lastKeyAt: CFTimeInterval = -1e9
    private var keyAfterButton = false
    private var mouseDx = 0.0, mouseDy = 0.0        // movement since the last tick (for the owner)
    private var lastMoveAt: CFTimeInterval = 0
    private var pendingLockDx = 0.0, pendingLockDy = 0.0

    // View state.
    private var screen = Sz(1920, 1080)
    private var center = Pt(960, 540)
    private var virtualPtr = Pt(960, 540)       // the pointer as the view sees it (deltas while locked)
    private var springVel = (x: 0.0, y: 0.0)
    private var zoomedSession = false
    private var lastWritten: (c: Pt, level: Double)?
    private var storedQuickLevel = 0.0
    private(set) var inspect = false
    private var inspectAnchor = CGPoint.zero
    private var returning = false               // the view is gliding back to the pointer
    private var wasLocked = false
    private var paused = false                  // the user switched Wind off from the menu
    private var simHeld = Set<Action>()         // holds from the command channel (tests)

    var onLevelChange: ((Double) -> Void)?
    var onQuitRequested: (() -> Void)?
    private var lastReportedLevel = 1.0

    var level: Double { zoom.level }
    var tapActive: Bool { tap.map { CGEvent.tapIsEnabled(tap: $0) } ?? false }
    var isLocked: Bool { lock.locked }
    var viewOwner: ViewOwner { owner.owner }
    var currentCenter: Pt { center }
    var isPaused: Bool { paused }

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
        SkyLight.reset()
        ColorFilter.restore()
    }

    func setPaused(_ p: Bool) {
        paused = p
        if p {
            _ = router.releaseAll()
            zoom.reset()
            endInspect()
            SkyLight.reset(); lastWritten = nil; zoomedSession = false
            reportLevel()
        }
    }

    private func updateScreen() {
        let b = CGDisplayBounds(CGMainDisplayID())
        screen = Sz(b.width, b.height)
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
            for a in router.releaseAll() { edge(.up(a)) }
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
            buttonsDown += 1; lastButtonAt = now; keyAfterButton = false
            return pass
        case .leftMouseUp, .rightMouseUp:
            buttonsDown = max(0, buttonsDown - 1); lastButtonAt = now
            return pass

        case .otherMouseDown:
            let n = Int(e.getIntegerValueField(.mouseEventButtonNumber))
            let (sw, ed) = router.triggerDown(.mouse(n), mods: mods)
            if let ed { edge(ed); return nil }
            if sw { return nil }
            buttonsDown += 1; lastButtonAt = now; keyAfterButton = false
            return pass
        case .otherMouseUp:
            let n = Int(e.getIntegerValueField(.mouseEventButtonNumber))
            let (sw, ed) = router.triggerUp(.mouse(n))
            if let ed { edge(ed) }
            if sw { return nil }
            buttonsDown = max(0, buttonsDown - 1); lastButtonAt = now
            return pass

        case .mouseMoved, .leftMouseDragged, .rightMouseDragged, .otherMouseDragged:
            let dx = Double(e.getIntegerValueField(.mouseEventDeltaX))
            let dy = Double(e.getIntegerValueField(.mouseEventDeltaY))
            let loc = e.location
            let dt = (now - lastMoveAt) * 1000
            lastMoveAt = now
            mouseDx += dx; mouseDy += dy
            let pinned = loc.x <= 0 || loc.y <= 0 || loc.x >= screen.w - 1 || loc.y >= screen.h - 1
            lock.feed(dx: dx, dy: dy, pos: Pt(loc.x, loc.y), dtMs: dt, pinned: pinned)
            pendingLockDx += dx; pendingLockDy += dy
            if zoom.level > 1.0001 || inspect { wake() }
            return pass

        case .scrollWheel:
            guard let want = config.wheelMods, modsMatch(mods, want) else { return pass }
            let continuous = e.getIntegerValueField(.scrollWheelEventIsContinuous) != 0
            var steps: Double
            if continuous {
                steps = e.getDoubleValueField(.scrollWheelEventPointDeltaAxis1) / 12.0
            } else {
                steps = e.getDoubleValueField(.scrollWheelEventFixedPtDeltaAxis1)
                steps = max(-4, min(4, steps))
            }
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
        inspectAnchor = CGEvent(source: nil)?.location ?? .zero
        CGAssociateMouseAndMouseCursorPosition(0)
        Log.info("inspect on at \(Int(inspectAnchor.x)),\(Int(inspectAnchor.y))")
    }

    private func endInspect() {
        guard inspect else { return }
        inspect = false
        CGAssociateMouseAndMouseCursorPosition(1)
        // The view glides back to the pointer (warps are unreliable under the compositor zoom).
        returning = true
        Log.info("inspect off")
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

        let ptr = CGEvent(source: nil)?.location ?? CGPoint(x: center.x, y: center.y)
        let ldx = pendingLockDx, ldy = pendingLockDy
        pendingLockDx = 0; pendingLockDy = 0

        if lv <= 1.0001 && !zoom.isMoving {
            if zoomedSession {
                SkyLight.reset()
                zoomedSession = false
                lastWritten = nil
                endInspect()
                Log.info("zoom session end")
            }
            mouseDx = 0; mouseDy = 0
            reportLevel()
            if !held(.panLeft) && !held(.panRight) && !held(.panUp) && !held(.panDown) {
                l.isPaused = true
            }
            return
        }

        if !zoomedSession {
            zoomedSession = true
            updateScreen()
            center = clampCenter(Pt(ptr.x, ptr.y), level: lv, screen: screen)
            virtualPtr = Pt(ptr.x, ptr.y)
            springVel = (0, 0)
            returning = false
            resetViewOwnerForSession(&owner)
            lock.reset()
            Log.info("zoom session start at \(Int(ptr.x)),\(Int(ptr.y))")
        }

        // Who owns the view this tick.
        let locked = config.lockTracking && lock.locked
        if wasLocked && !locked { returning = true }
        if locked != wasLocked { Log.info(locked ? "game lock: panning from mouse deltas" : "game lock released") }
        wasLocked = locked
        var oi = ViewOwnerInputs()
        oi.enabled = !locked && !inspect
        oi.trackCaret = config.trackCaret
        oi.trackFocus = config.trackFocus
        oi.mouseDx = mouseDx; oi.mouseDy = mouseDy
        oi.buttonDown = buttonsDown > 0
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
        if owner.owner != prevOwner { Log.verbose("owner \(prevOwner) -> \(owner.owner)") }

        var target = center
        if inspect {
            // The pointer is frozen; the hand pans the view.
            virtualPtr = Pt(virtualPtr.x + ldx * config.cursorSensitivity, virtualPtr.y + ldy * config.cursorSensitivity)
            virtualPtr = Pt(min(max(virtualPtr.x, 0), screen.w), min(max(virtualPtr.y, 0), screen.h))
            target = clampCenter(virtualPtr, level: lv, screen: screen)
        } else if locked {
            virtualPtr = Pt(virtualPtr.x + ldx * config.cursorSensitivity, virtualPtr.y + ldy * config.cursorSensitivity)
            virtualPtr = Pt(min(max(virtualPtr.x, 0), screen.w), min(max(virtualPtr.y, 0), screen.h))
            target = mouseTarget(virtualPtr, lv)
        } else {
            switch owner.owner {
            case .mouse:
                // The mouse took the view back from the caret, keys or Inspect: the view glides
                // back to the pointer. (Wind warps the pointer to the view instead; a warp while
                // zoomed is reinterpreted in screen space at the next zoom write, so it lands wrong.)
                if owner.warpPointer || prevOwner != .mouse { returning = true }
                virtualPtr = Pt(ptr.x, ptr.y)
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
        write(lv)
        reportLevel()
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
