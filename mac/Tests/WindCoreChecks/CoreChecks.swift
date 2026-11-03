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

    func stillViewHoldsThenRecentres() {
        let cur = Pt(960, 540)
        // at 3x the view is 640 x 360 (x 640...1280); a 15% margin leaves x 736...1184, y 414...666
        expect(stillViewTarget(cur, cur: cur, ptr: Pt(1100, 600), level: 3, screen: screen, marginPct: 15) == cur)
        expect(stillViewTarget(cur, cur: cur, ptr: Pt(1200, 600), level: 3, screen: screen, marginPct: 15) == Pt(1200, 540))
        // a glide under way keeps its target while the pointer is back inside the band
        expect(stillViewTarget(Pt(1200, 540), cur: Pt(1100, 540), ptr: Pt(1210, 560), level: 3, screen: screen, marginPct: 15) == Pt(1200, 540))
        // at a screen edge the target stays on the clamp
        expect(stillViewTarget(Pt(1600, 900), cur: Pt(1600, 900), ptr: Pt(1915, 1075), level: 3, screen: screen, marginPct: 15) == Pt(1600, 900))
        // with a lead the view goes past the pointer, toward where it is heading
        expect(stillViewTarget(cur, cur: cur, ptr: Pt(1200, 600), level: 3, screen: screen, marginPct: 15, leadPct: 20) == Pt(1328, 540))
        expect(stillViewTarget(cur, cur: cur, ptr: Pt(900, 400), level: 3, screen: screen, marginPct: 15, leadPct: 20) == Pt(960, 328))
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
    func hiddenWhileMovingLocks() {
        var d = LockDetector()
        for _ in 0..<5 { d.feed(cursorVisible: false, motion: 6) }
        expect(d.locked)
        d.feed(cursorVisible: true, motion: 6)
        expect(!d.locked)
    }

    func typingHideDoesNotLock() {
        // Typing hides the cursor; the first move shows it again.
        var d = LockDetector()
        d.feed(cursorVisible: false, motion: 0)
        d.feed(cursorVisible: false, motion: 7)
        d.feed(cursorVisible: true, motion: 7)
        for _ in 0..<10 { d.feed(cursorVisible: true, motion: 7) }
        expect(!d.locked)
    }

    func hiddenButFollowingTheHandIsNotALock() {
        // After typing macOS may report the cursor hidden while it moves with the hand.
        var d = LockDetector()
        for _ in 0..<20 { d.feed(cursorVisible: false, motion: 8, pointerMoved: 3) }
        expect(!d.locked)
        for _ in 0..<5 { d.feed(cursorVisible: false, motion: 8, pointerMoved: 0) }
        expect(d.locked)
        d.feed(cursorVisible: false, motion: 8, pointerMoved: 3)
        expect(d.locked)                                   // one tick could be an echo
        d.feed(cursorVisible: false, motion: 8, pointerMoved: 3)
        expect(!d.locked)
    }

    func hiddenButStillIsNotALock() {
        var d = LockDetector()
        for _ in 0..<100 { d.feed(cursorVisible: false, motion: 0) }
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
        expect(bindProblem(parseBind("opt+a")!) != nil)
        expect(bindProblem(parseBind("ctrl+opt+a")!) == nil)
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

    /// The shared rule cases (Tests/fixtures/mac_keybind_cases.txt), also run by the settings page's
    /// `npm run test:rules` against ui/src/lib/keybindRules.js.
    func sharedRuleCases() throws {
        let url = URL(fileURLWithPath: #filePath).deletingLastPathComponent().deletingLastPathComponent()
            .appendingPathComponent("fixtures/mac_keybind_cases.txt")
        let text = try String(contentsOf: url, encoding: .utf8)
        var n = 0
        for raw in text.split(whereSeparator: \.isNewline) {
            let line = raw.trimmingCharacters(in: .whitespaces)
            if line.isEmpty || line.hasPrefix("#") { continue }
            guard let sp = line.lastIndex(of: " ") else { expect(false); continue }
            let bind = line[..<sp].trimmingCharacters(in: .whitespaces)
            let want = String(line[line.index(after: sp)...])
            let got = parseBind(bind).map { bindVerdict($0).rawValue } ?? "invalid"
            if got != want { print("  case \"\(bind)\": got \(got), want \(want)") }
            expect(got == want)
            n += 1
        }
        expect(n > 100)
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

    /// renderScaling keeps Windows' values (0-2) and the Mac's own (3, 4); anything else is Nearest.
    func renderScalingClampsLikeWindows() throws {
        for v in 0...4 { expect(try Config.load(from: Data("{\"renderScaling\": \(v)}".utf8)).renderScaling == v) }
        expect(try Config.load(from: Data(#"{"renderScaling": 5}"#.utf8)).renderScaling == 0)
        expect(try Config.load(from: Data(#"{"renderScaling": -1}"#.utf8)).renderScaling == 0)
        expect(Config().renderScaling == RenderScaling.nearest.rawValue)
        expect(RenderScaling.xbr.usesShader && RenderScaling.nis.usesShader && !RenderScaling.sharp.usesShader)
        expect(Set(RenderScaling.menuOrder) == Set(RenderScaling.allCases))
    }

    /// A file from before 0.4.0 (smoothImages set both engines) keeps Render smooth; a file that names
    /// renderScaling keeps it, and Transform's smoothImages stays its own.
    func smoothImagesMigratesToRenderScaling() throws {
        let old = try Config.load(from: Data(#"{"smoothImages": true}"#.utf8))
        expect(old.renderScaling == RenderScaling.smooth.rawValue && old.smoothImages)
        expect(try Config.load(from: Data(#"{"smoothImages": false}"#.utf8)).renderScaling == 0)
        let both = try Config.load(from: Data(#"{"smoothImages": true, "renderScaling": 2}"#.utf8))
        expect(both.renderScaling == 2 && both.smoothImages)
        let saved = try Config.load(from: Config.load(from: Data(#"{"smoothImages": true, "renderScaling": 0}"#.utf8)).encoded())
        expect(saved.renderScaling == 0)
    }

    /// Only zoom in and out take several binds (up to three); every other action keeps its first.
    func clampsBindCounts() throws {
        let c = try Config.load(from: Data(#"{"zoomIn": ["mouse4", "f13", "f14", "f15"], "quit": ["cmd+escape", "f16"], "panLeft": ["ctrl+opt+left", "f17"]}"#.utf8))
        expect(c.zoomIn == ["mouse4", "f13", "f14"])
        expect(c.quit == ["cmd+escape"])
        expect(c.panLeft == ["ctrl+opt+left"])
    }

    func defaultBindsAreAllValid() {
        expect(Config().binds().problems.isEmpty)
        expect(Config().wheelMods == [.ctrl])
    }
}

/// Tests/fixtures/config_defaults.json: every Config key with its type, its default as the settings
/// page sees it (SettingsWeb.values: bools "1"/"0", lists comma-joined) and, for numbers, the range
/// clamp() allows (probed by loading a huge and a tiny value); for strings, what clamp() turns an
/// unknown value into. ui/tests/config-defaults.nodetest.mjs checks the settings schema against it.
/// Regenerate with `swift run wind-checks --write-defaults`; the check below fails when it is stale.
enum DefaultsFixture {
    static let url = URL(fileURLWithPath: #filePath).deletingLastPathComponent().deletingLastPathComponent()
        .appendingPathComponent("fixtures/config_defaults.json")

    static func num(_ x: Double) -> String { x == x.rounded() && abs(x) < 1e15 ? String(Int(x)) : String(x) }
    static func str(_ s: String) -> String {
        let d = try! JSONSerialization.data(withJSONObject: [s], options: [.fragmentsAllowed])
        return String(String(data: d, encoding: .utf8)!.dropFirst().dropLast())
    }
    static func dict(_ c: Config) -> [String: Any] {
        try! JSONSerialization.jsonObject(with: c.encoded()) as! [String: Any]
    }
    /// The value `key` ends up with after loading `json` (a JSON value) and clamp().
    static func probe(_ key: String, _ json: String) -> Any? {
        guard let c = try? Config.load(from: Data("{\"\(key)\": \(json)}".utf8)) else { return nil }
        return dict(c)[key]
    }

    static func generate() -> String {
        let defs = dict(Config())
        // Int or Double from the Swift property type (JSON writes 30.0 as 30, so the number cannot tell).
        let intKeys = Set(Mirror(reflecting: Config()).children.compactMap { $0.value is Int ? $0.label : nil })
        var lines: [String] = []
        for k in defs.keys.sorted() {
            let v = defs[k]!
            var f: [String] = []
            if let n = v as? NSNumber {
                if CFGetTypeID(n) == CFBooleanGetTypeID() {
                    f = ["\"type\": \"bool\"", "\"def\": \"\(n.boolValue ? "1" : "0")\""]
                } else {
                    let isInt = intKeys.contains(k)
                    let lo = (probe(k, isInt ? "-1000000" : "-1e9") as? NSNumber)?.doubleValue ?? .nan
                    let hi = (probe(k, isInt ? "1000000" : "1e9") as? NSNumber)?.doubleValue ?? .nan
                    f = ["\"type\": \"\(isInt ? "int" : "double")\"", "\"def\": \"\(num(n.doubleValue))\"",
                         "\"min\": \(num(lo))", "\"max\": \(num(hi))"]
                }
            } else if let a = v as? [String] {
                f = ["\"type\": \"list\"", "\"def\": \(str(a.joined(separator: ",")))"]
            } else if let s = v as? String {
                let fb = probe(k, "\"__unknown__\"") as? String ?? ""
                f = ["\"type\": \"string\"", "\"def\": \(str(s))", "\"fallback\": \(str(fb))"]
            }
            lines.append("    \(str(k)): { " + f.joined(separator: ", ") + " }")
        }
        let palettes = Config.palettes.map(str).joined(separator: ", ")
        return "{\n  \"generatedBy\": \"swift run wind-checks --write-defaults\",\n  \"palettes\": [\(palettes)],\n  \"keys\": {\n"
            + lines.joined(separator: ",\n") + "\n  }\n}\n"
    }

    static func write() throws { try generate().write(to: url, atomically: true, encoding: .utf8) }

    static func isCurrent() {
        let have = (try? String(contentsOf: url, encoding: .utf8)) ?? ""
        if have != generate() { print("  Tests/fixtures/config_defaults.json is stale: run swift run wind-checks --write-defaults") }
        expect(have == generate())
    }
}

struct ShotTests {
    func hotkeysFollowTheUsersSettings() {
        let def = screenshotAreaHotkeys(nil)
        expect(def == [ShotHotkey(keyCode: 21, mods: [.cmd, .shift], clipboard: false), ShotHotkey(keyCode: 21, mods: [.cmd, .shift, .ctrl], clipboard: true)])
        // 30 switched off, 31 moved to Cmd+Opt+5 (0x180000): Wind takes only what macOS would.
        let t: [String: Any] = ["30": ["enabled": false, "value": ["parameters": [52, 21, 1179648], "type": "standard"]],
                                "31": ["enabled": true, "value": ["parameters": [53, 23, 0x180000], "type": "standard"]]]
        expect(screenshotAreaHotkeys(t) == [ShotHotkey(keyCode: 23, mods: [.cmd, .opt], clipboard: true)])
    }

    func rectRoundsToNearestPoints() {
        let s = Sz(1920, 1080)
        let r = shotRect(anchor: Pt(100.4, 200.6), end: Pt(50.2, 300.49), screen: s)
        expect(r.map { [$0.x, $0.y, $0.w, $0.h] } == [50, 201, 50, 99])
        expect(shotRect(anchor: Pt(10.2, 10.2), end: Pt(10.4, 40), screen: s) == nil)   // a click, not a drag
        let e = shotRect(anchor: Pt(1900, 1000), end: Pt(1990, 1200), screen: s)          // clamped to the screen
        expect(e.map { [$0.x, $0.y, $0.w, $0.h] } == [1900, 1000, 20, 80])
    }
}
