import Foundation

@testable import WindCore

struct ZoomTests {
    func holdZoomsInAtTheBaseRate() {
        var z = ZoomController(maxLevel: 30)
        z.setEaseOut(seconds: 0)
        z.direction = .zoomIn
        for _ in 0..<120 { z.tick(1.0 / 120) }
        // 1 s at speed 1.0 = 2.5 doublings
        expect(abs(z.level - pow(2, 2.5)) < 0.01)
    }

    func releaseEasesOutThenStops() {
        var z = ZoomController(maxLevel: 30)
        z.direction = .zoomIn
        for _ in 0..<60 { z.tick(1.0 / 120) }
        z.direction = .none
        let atRelease = z.level
        z.tick(1.0 / 120)
        expect(z.level > atRelease)          // still gliding
        for _ in 0..<120 { z.tick(1.0 / 120) }
        expect(!z.isMoving)
    }

    func clampsAtMax() {
        var z = ZoomController(maxLevel: 4)
        z.direction = .zoomIn
        for _ in 0..<600 { z.tick(1.0 / 120) }
        expect(z.level == 4)
    }

    func zoomOutStopsAtOne() {
        var z = ZoomController(maxLevel: 30)
        z.setLevel(8)
        z.direction = .zoomOut
        for _ in 0..<600 { z.tick(1.0 / 120) }
        z.direction = .none
        z.tick(1.0 / 120)
        expect(z.level == 1)
        expect(!z.isMoving)
    }

    func wheelGlidesToTarget() {
        var z = ZoomController(maxLevel: 30)
        z.wheelNotches(3)
        let want = pow(1 + wheelNotchStep(speed: 1), 3)
        for _ in 0..<240 { z.tick(1.0 / 120) }
        expect(abs(z.level - want) < 1e-9)
        expect(!z.hasTarget)
    }

    func bothHeldFreezes() {
        expect(resolveDirection(inHeld: true, outHeld: true) == .none)
        expect(resolveDirection(inHeld: true, outHeld: false) == .zoomIn)
    }

    func quickZoomToggles() {
        expect(applyQuickZoom(cur: 1, stored: 0, def: 4, maxLevel: 30) == QuickZoomResult(newLevel: 4, newStored: 0))
        expect(applyQuickZoom(cur: 6, stored: 4, def: 4, maxLevel: 30) == QuickZoomResult(newLevel: 1, newStored: 6))
        expect(applyQuickZoom(cur: 1.5, stored: 4, def: 4, maxLevel: 30) == QuickZoomResult(newLevel: 1, newStored: 4))
        expect(applyQuickZoom(cur: 1, stored: 40, def: 4, maxLevel: 30).newLevel == 30)
    }
}

struct ViewTests {
    let screen = Sz(1920, 1080)

    func centreClampsToScreen() {
        // The Wind formula: offset = clamp(cursor - screen/(2*level), 0, screen - screen/level)
        let c = clampCenter(Pt(10, 1070), level: 4, screen: screen)
        expect(c == Pt(240, 945))
        expect(clampCenter(Pt(500, 500), level: 1, screen: screen) == Pt(960, 540))
    }

    func edgePanMovesOnlyPastTheBand() {
        let cur = Pt(960, 540)
        // at 2x the view is 960 x 540: x in [480, 1440]
        expect(edgePanCenter(cur: cur, ptr: Pt(1000, 600), level: 2, screen: screen, marginPct: 0) == cur)
        let moved = edgePanCenter(cur: cur, ptr: Pt(1500, 540), level: 2, screen: screen, marginPct: 0)
        expect(moved == Pt(1020, 540))
    }

    func springConverges() {
        var v = 0.0, x = 0.0
        for _ in 0..<240 { x = springToward(x, 100, vel: &v, dtMs: 1000.0 / 120, glideMs: 200) }
        expect(abs(x - 100) < 0.01)
    }

    func trackTargetRejectsBogusRects() {
        expect(trackTargetCenter(Rc(l: 0, t: 0, r: 0, b: 0), cur: Pt(960, 540), level: 2, screen: screen, align: 0, marginPct: 15) == nil)
        expect(trackTargetCenter(Rc(l: 3000, t: 10, r: 3001, b: 20), cur: Pt(960, 540), level: 2, screen: screen, align: 0, marginPct: 15) == nil)
        let c = trackTargetCenter(Rc(l: 700, t: 300, r: 702, b: 318), cur: Pt(960, 540), level: 2, screen: screen, align: 0, marginPct: 15)
        expect(c == Pt(701, 309))
    }
}

struct LockTests {
    func deltasWithoutTravelLock() {
        var d = LockDetector()
        for _ in 0..<10 { d.feed(dx: 5, dy: 0, pos: Pt(500, 500), dtMs: 8, pinned: false) }
        expect(d.locked)
        // pointer moves with the hand again: unlock
        var x = 500.0
        for _ in 0..<10 { x += 5; d.feed(dx: 5, dy: 0, pos: Pt(x, 500), dtMs: 8, pinned: false) }
        expect(!d.locked)
    }

    func normalMovementNeverLocks() {
        var d = LockDetector()
        var x = 100.0
        for _ in 0..<50 { x += 4; d.feed(dx: 4, dy: 0, pos: Pt(x, 300), dtMs: 8, pinned: false) }
        expect(!d.locked)
    }

    func zoomedTravelIsJudgedAt1x() {
        // At 6x the pointer moves 1/6 as far: that is normal movement, not a lock, and a lock
        // that is held releases once the pointer moves with the hand again.
        var d = LockDetector()
        var x = 100.0
        for _ in 0..<50 { x += 1; d.feed(dx: 6, dy: 0, pos: Pt(x, 300), dtMs: 8, pinned: false, level: 6) }
        expect(!d.locked)
        for _ in 0..<10 { d.feed(dx: 6, dy: 0, pos: Pt(x, 300), dtMs: 8, pinned: false, level: 6) }
        expect(d.locked)
        for _ in 0..<10 { x += 1; d.feed(dx: 6, dy: 0, pos: Pt(x, 300), dtMs: 8, pinned: false, level: 6) }
        expect(!d.locked)
    }

    func pinnedAtEdgeIsNotALock() {
        var d = LockDetector()
        for _ in 0..<20 { d.feed(dx: 6, dy: 0, pos: Pt(1919, 300), dtMs: 8, pinned: true) }
        expect(!d.locked)
    }
}

struct OwnerTests {
    func inputs(_ snap: TrackSnapshot, msSinceKey: Double = 100) -> ViewOwnerInputs {
        var i = ViewOwnerInputs()
        i.enabled = true; i.trackCaret = true; i.dtMs = 8; i.snap = snap; i.msSinceKey = msSinceKey
        return i
    }

    func typedCaretTakesTheViewAndMouseTakesItBack() {
        var s = ViewOwnerState()
        resetViewOwnerForSession(&s)
        stepViewOwner(&s, inputs(TrackSnapshot(kind: .caret, seq: 1, rect: Rc(l: 1, t: 1, r: 2, b: 10))))
        expect(s.owner == .mouse)             // first tick re-baselines
        stepViewOwner(&s, inputs(TrackSnapshot(kind: .caret, seq: 2, rect: Rc(l: 5, t: 1, r: 6, b: 10))))
        expect(s.owner == .caret)
        var i = inputs(TrackSnapshot(kind: .caret, seq: 2))
        i.mouseDx = 4
        stepViewOwner(&s, i)
        expect(s.owner == .mouse)
        expect(s.warpPointer)
    }

    func caretWithoutAKeyIsIgnored() {
        var s = ViewOwnerState()
        resetViewOwnerForSession(&s)
        stepViewOwner(&s, inputs(TrackSnapshot(kind: .caret, seq: 1)))
        stepViewOwner(&s, inputs(TrackSnapshot(kind: .caret, seq: 2, rect: Rc(l: 5, t: 1, r: 6, b: 10)), msSinceKey: 5000))
        expect(s.owner == .mouse)
    }
}

struct BindTests {
    func parseAndFormat() {
        expect(parseBind("ctrl+opt+equal") == Bind(mods: [.ctrl, .opt], trigger: .key(0x18)))
        expect(parseBind("Mouse4") == Bind(mods: [], trigger: .mouse(4)))
        expect(parseBind("ctrl") == Bind(mods: [.ctrl], trigger: .none))
        expect(parseBind("ctrl+nope") == nil)
        expect(parseBind("mouse1") == nil)
        expect(formatBind(parseBind("opt+ctrl+left")!) == "ctrl+opt+left")
        expect(formatBind(parseBind("cmd+esc")!) == "cmd+escape")
    }

    func rules() {
        expect(bindProblem(parseBind("a")!) != nil)
        expect(bindProblem(parseBind("shift+a")!) != nil)
        expect(bindProblem(parseBind("cmd+c")!) != nil)
        expect(bindProblem(parseBind("ctrl+opt+z")!) == nil)
        expect(bindProblem(parseBind("f13")!) == nil)
        expect(bindProblem(parseBind("mouse3")!) == nil)
    }

    func routerHoldsAndSwallowsBalanced() {
        var r = InputRouter()
        r.setBinds([.zoomIn: [parseBind("ctrl+opt+equal")!], .zoomOut: [parseBind("mouse3")!]])
        let (sw1, e1) = r.triggerDown(.key(0x18), mods: [.ctrl, .opt])
        expect(sw1 && e1 == .down(.zoomIn))
        expect(r.isHeld(.zoomIn))
        expect(r.triggerDown(.key(0x18), mods: [], isRepeat: true).0)   // repeat swallowed
        let (sw2, e2) = r.triggerUp(.key(0x18))                          // mods already released
        expect(sw2 && e2 == .up(.zoomIn))
        expect(!r.triggerUp(.key(0x18)).0)                               // nothing else swallowed
        expect(!r.triggerDown(.key(0x18), mods: [.ctrl]).0)              // wrong mods pass through
        expect(r.triggerDown(.mouse(3), mods: []).1 == .down(.zoomOut))
    }

    func timeoutStopsButKeepsSwallowing() {
        var r = InputRouter()
        r.setBinds([.zoomIn: [parseBind("ctrl+opt+equal")!]])
        _ = r.triggerDown(.key(0x18), mods: [.ctrl, .opt])
        expect(r.stopAll() == [.zoomIn])
        expect(!r.isHeld(.zoomIn))
        expect(r.triggerDown(.key(0x18), mods: [], isRepeat: true) == (true, nil))   // repeat still swallowed
        expect(r.triggerUp(.key(0x18)) == (true, nil))                               // up swallowed, no edge
        expect(r.triggerUp(.key(0x18)).0 == false)
    }

    func plusKeyBinds() {
        expect(parseBind("ctrl++") == Bind(mods: [.ctrl], trigger: .key(0x18)))
        expect(parseBind("ctrl+") == nil)
        expect(parseBind("ctrl++opt") == nil)
    }
}

struct ConfigTests {
    func partialFileKeepsDefaults() throws {
        let c = try Config.load(from: Data(#"{"maxLevel": 12, "zoomIn": "f13", "bogus": 1}"#.utf8))
        expect(c.maxLevel == 12)
        expect(c.zoomIn == ["f13"])
        expect(c.zoomOut == Config().zoomOut)
    }

    func wrongTypeDropsOnlyThatKey() throws {
        var dropped: [String] = []
        let c = try Config.load(from: Data(#"{"maxLevel": "big", "zoomInSpeed": 2}"#.utf8), dropped: &dropped)
        expect(dropped == ["maxLevel"])
        expect(c.maxLevel == Config().maxLevel)
        expect(c.zoomInSpeed == 2)
    }

    func clampsValues() throws {
        let c = try Config.load(from: Data(#"{"maxLevel": 1000, "zoomInSpeed": 0}"#.utf8))
        expect(c.maxLevel == 64)
        expect(c.zoomInSpeed == 0.25)
    }

    func defaultBindsAreAllValid() {
        expect(Config().binds().problems.isEmpty)
        expect(Config().wheelMods == [.ctrl])
    }
}
