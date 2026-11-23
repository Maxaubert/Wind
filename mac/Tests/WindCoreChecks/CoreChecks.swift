// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
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

    /// renderScaling keeps Windows' 0 (Nearest) and 1 (xBR); anything else, Windows' 2 (NIS) and the removed
    /// Smooth and Sharp (3, 4) included, is Nearest. smoothImages (Transform's) does not touch it.
    func renderScalingClampsLikeWindows() throws {
        for v in 0...1 { expect(try Config.load(from: Data("{\"renderScaling\": \(v)}".utf8)).renderScaling == v) }
        for v in [2, 3, 4, 5, -1] { expect(try Config.load(from: Data("{\"renderScaling\": \(v)}".utf8)).renderScaling == 0) }
        expect(Config().renderScaling == RenderScaling.nearest.rawValue)
        expect(RenderScaling.xbr.usesShader && !RenderScaling.nearest.usesShader)
        let old = try Config.load(from: Data(#"{"smoothImages": true}"#.utf8))
        expect(old.renderScaling == 0 && old.smoothImages)
        let both = try Config.load(from: Data(#"{"smoothImages": true, "renderScaling": 1}"#.utf8))
        expect(both.renderScaling == 1 && both.smoothImages)
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
        expect(Config().wheelModsAll == [[.ctrl]])
    }

    func secondWheelModsAlsoZoom() throws {
        let c = try Config.load(from: Data(#"{"wheelZoomMods": "opt", "wheelZoomMods2": "ctrl"}"#.utf8))
        expect(c.wheelModsAll == [[.opt], [.ctrl]])
        let off = try Config.load(from: Data(#"{"wheelZoomMods": "", "wheelZoomMods2": "cmd+a"}"#.utf8))
        expect(off.wheelModsAll.isEmpty)
        let bad = try Config.load(from: Data(#"{"wheelZoomMods": "opt", "wheelZoomMods2": "cmd+a"}"#.utf8))
        expect(bad.wheelModsAll == [[.opt]])      // a key in a wheel bind is not a modifier set
        let second = try Config.load(from: Data(#"{"wheelZoomMods": "", "wheelZoomMods2": "ctrl"}"#.utf8))
        expect(second.wheelModsAll.isEmpty)       // the second is an "or" to the first
    }

    func wheelZoomSpeedRange() throws {
        expect(try Config.load(from: Data(#"{"wheelZoomSpeed": 3}"#.utf8)).wheelZoomSpeed == 0.5)
        expect(try Config.load(from: Data(#"{"wheelZoomSpeed": 0.01}"#.utf8)).wheelZoomSpeed == 0.05)
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

/// The screen colour is session-only (#601): reset at a plain start, kept by a self-triggered relaunch.
struct SessionColourTests {
    func json(_ d: Data) -> [String: Any] { (try? JSONSerialization.jsonObject(with: d)) as? [String: Any] ?? [:] }

    func plainStartResetsOnlyTheColour() {
        let src = Data(#"{"warmthPct": 40, "dimPct": 55, "maxLevel": 12.5, "engine": "draw", "futureKey": "x"}"#.utf8)
        guard let r = SessionColour.reset(src) else { expect(false); return }
        let o = json(r.data)
        expect((o["warmthPct"] as? Double) == 0 && (o["dimPct"] as? Double) == 100)
        expect((o["maxLevel"] as? Double) == 12.5 && (o["engine"] as? String) == "draw" && (o["futureKey"] as? String) == "x")
        expect(Set(r.was.keys) == ["warmthPct", "dimPct"])
        expect(SessionColour.keys == ["warmthPct", "dimPct", "satPct"])   // Windows' kSessionKeys (src/config/profiles.h)
        let c = try? Config.load(from: r.data)
        expect(c?.warmthPct == 0 && c?.dimPct == 100 && c?.maxLevel == 12.5)
    }

    func nothingToResetWritesNothing() {
        expect(SessionColour.reset(Data(#"{"warmthPct": 0, "dimPct": 100, "maxLevel": 8}"#.utf8)) == nil)
        expect(SessionColour.reset(Data(#"{"maxLevel": 8}"#.utf8)) == nil)          // absent keys read as the defaults
        expect(SessionColour.reset(Data("not json".utf8)) == nil)                     // an unreadable file is left alone
        expect(SessionColour.reset(Data("[1, 2]".utf8)) == nil)
        // A wrong type (it would load as the default anyway) is replaced by the default.
        let r = SessionColour.reset(Data(#"{"warmthPct": "warm"}"#.utf8))
        expect(r != nil && (json(r!.data)["warmthPct"] as? Double) == 0)
    }

    func relaunchKeepsTheColour() {
        let now = Date(timeIntervalSince1970: 1_000_000)
        expect(!SessionColour.keepsColour(arguments: ["/Applications/Wind.app/Contents/MacOS/Wind"], quitMarker: nil, now: now))
        expect(SessionColour.keepsColour(arguments: ["Wind", SessionColour.relaunchArgument], quitMarker: nil, now: now))
        expect(SessionColour.keepsColour(arguments: ["Wind"], quitMarker: now.addingTimeInterval(-3), now: now))
        expect(!SessionColour.keepsColour(arguments: ["Wind"], quitMarker: now.addingTimeInterval(-SessionColour.markerWindow - 1), now: now))
        expect(!SessionColour.keepsColour(arguments: ["Wind"], quitMarker: now.addingTimeInterval(30), now: now))   // clock moved back
        expect(!SessionColour.quitKeepsColour(permissionAskedAt: nil, now: now))
        expect(SessionColour.quitKeepsColour(permissionAskedAt: now.addingTimeInterval(-60), now: now))
        expect(!SessionColour.quitKeepsColour(permissionAskedAt: now.addingTimeInterval(-SessionColour.permissionWindow - 1), now: now))
    }
}

/// Saturation (#599): the overlay's Core Animation matrix is saturationMatrix transposed (CA rows are outputs);
/// the matrix values themselves are the shared cases (color_cases.txt, sat rows).
struct SaturationTests {
    func caMatrixIsTheSharedMatrixTransposed() {
        for s in [0.2, 0.5, 1.0, 1.25, 1.5] {
            let m = saturationMatrix(s), f = caColorMatrix(s)
            for out in 0..<3 { for inp in 0..<3 { expect(abs(Double(f[out * 5 + inp]) - m[inp][out]) < 1e-6) } }
            for out in 0..<3 { expect(f[out * 5 + 3] == 0 && f[out * 5 + 4] == 0) }      // no alpha mix, no offset
            expect(f[15...19] == [0, 0, 0, 1, 0])                                       // alpha passes through
            // Grey stays grey (each output row sums to 1), and a grey's luma is kept.
            for out in 0..<3 { expect(abs(Double(f[out * 5] + f[out * 5 + 1] + f[out * 5 + 2]) - 1) < 1e-6) }
        }
        expect(caColorMatrix(1) == [1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0])
    }

    func neutralOnlyAt100() {
        expect(saturationIsNeutral(100) && !saturationIsNeutral(95) && !saturationIsNeutral(105))
        expect(try! Config.load(from: Data(#"{"satPct": 5}"#.utf8)).satPct == 20)
        expect(try! Config.load(from: Data(#"{"satPct": 400}"#.utf8)).satPct == 150)
        expect(Config().satPct == 100)
    }
}

/// Config's menu bar keys and MenuLayout, the parser the menu bar panel uses. The parse cases are shared
/// with the settings page (mac/ui/tests/menu-layout.nodetest.mjs).
struct MenuLayoutTests {
    func defaults() {
        let c = Config()
        expect(c.menuPerf)
        expect(MenuLayout.shown(MenuLayout.sliders(c)) == ["warmthPct", "dimPct"])
        expect(MenuLayout.sliders(c).map(\.key) == MenuLayout.sliders)
        expect(MenuLayout.shown(MenuLayout.toggles(c)) == MenuLayout.toggles)
    }

    func loadsFromTheFile() throws {
        let c = try Config.load(from: Data(#"{"menuPerf": false, "menuSliders": "maxLevel,panSpeed", "menuToggles": [], "menuSliderOrder": ["panSpeed", "bogus"]}"#.utf8))
        expect(!c.menuPerf)
        expect(MenuLayout.shown(MenuLayout.sliders(c)) == ["panSpeed", "maxLevel"])
        expect(MenuLayout.shown(MenuLayout.toggles(c)).isEmpty)
        var dropped: [String] = []
        let bad = try Config.load(from: Data(#"{"menuSliders": 3}"#.utf8), dropped: &dropped)
        expect(dropped == ["menuSliders"] && bad.menuSliders == MenuLayout.defaultSliders)
    }

    func serialiseRoundTrips() {
        let items = MenuLayout.parse(.sliders, enabled: ["satPct", "warmthPct"], order: ["maxLevel", "satPct"])
        let w = MenuLayout.serialise(items)
        expect(w.enabled == ["satPct", "warmthPct"])
        expect(MenuLayout.parse(.sliders, enabled: w.enabled, order: w.order) == items)
    }

    func sharedCases() throws {
        let url = DefaultsFixture.url.deletingLastPathComponent().appendingPathComponent("menu_layout_cases.json")
        let root = try JSONSerialization.jsonObject(with: Data(contentsOf: url)) as! [String: Any]
        let cases = root["cases"] as! [[String: Any]]
        expect(cases.count >= 10)
        let d = Config()
        for t in cases {
            let kind = MenuLayout.Kind(rawValue: t["kind"] as! String)!
            // null = the key is absent: Config's default. A string is what the page writes (one item, split).
            let enabled = (t["enabled"] as? String).map { [$0] } ?? (kind == .sliders ? d.menuSliders : d.menuToggles)
            let order = (t["order"] as? String).map { [$0] } ?? (kind == .sliders ? d.menuSliderOrder : d.menuToggleOrder)
            let items = MenuLayout.parse(kind, enabled: enabled, order: order)
            let gotOrder = items.map(\.key).joined(separator: ","), gotOn = MenuLayout.shown(items).joined(separator: ",")
            if gotOrder != t["wantOrder"] as! String || gotOn != t["wantOn"] as! String {
                print("  \(t["name"]!): got \(gotOrder) on \(gotOn)")
                expect(false)
            }
        }
    }
}

/// The menu bar panel's placement, its re-open guard and the Performance header's tick ring (MenuPanel.swift).
struct MenuPanelTests {
    // A 1440x900 screen, menu bar 25 pt: visibleFrame 0,0 1440x875 (no Dock).
    let vis = CGRect(x: 0, y: 0, width: 1440, height: 875)

    func cardRightEdgeUnderTheIcon() {
        let icon = CGRect(x: 1100, y: 875, width: 30, height: 25)
        let f = MenuPanel.frame(icon: icon, visible: vis, height: 400)
        let card = MenuPanel.card(in: f)
        expect(card.maxX == icon.maxX)
        expect(f.width == 328 && card.width == 300)
        expect(f.maxY == 872)                                  // 3 pt under the menu bar
        expect(f.height == 400 && card.minY == f.minY + 14)
    }

    func clampedToTheVisibleFrame() {
        // An icon at the far right: the card stops at the screen's right edge (only the clear margin outside).
        let right = MenuPanel.frame(icon: CGRect(x: 1430, y: 875, width: 30, height: 25), visible: vis, height: 300)
        expect(MenuPanel.card(in: right).maxX == 1440)
        // An icon near the left (a crowded menu bar, a notch): the card starts at the screen's left edge.
        let left = MenuPanel.frame(icon: CGRect(x: 20, y: 875, width: 30, height: 25), visible: vis, height: 300)
        expect(MenuPanel.card(in: left).minX == 0)
        // A second screen to the left of the main one (negative x), with its own menu bar.
        let v2 = CGRect(x: -1920, y: 0, width: 1920, height: 1055)
        let s2 = MenuPanel.frame(icon: CGRect(x: -400, y: 1055, width: 30, height: 25), visible: v2, height: 300)
        expect(MenuPanel.card(in: s2).maxX == -370 && s2.maxY == 1052)
        // Taller than the screen: cut at the bottom, the top stays under the menu bar.
        let tall = MenuPanel.frame(icon: CGRect(x: 1100, y: 875, width: 30, height: 25), visible: vis, height: 5000)
        expect(tall.maxY == 872 && tall.minY == -14)
    }

    func underAHiddenMenuBar() {
        // An auto-hiding menu bar (or a full-screen Space): visibleFrame reaches the screen top and the
        // revealed bar holds the icon, so the card hangs under the icon, not behind the bar.
        let full = CGRect(x: 0, y: 0, width: 1440, height: 900)
        let icon = CGRect(x: 1100, y: 876, width: 30, height: 24)
        let f = MenuPanel.frame(icon: icon, visible: full, height: 400)
        expect(f.maxY == 873 && MenuPanel.card(in: f).maxX == icon.maxX && f.height == 400)
        // Taller than the screen: cut at the bottom, the top still under the icon.
        let tall = MenuPanel.frame(icon: icon, visible: full, height: 5000)
        expect(tall.maxY == 873 && tall.minY == -14)
    }

    func reopenGuard() {
        expect(!MenuPanel.ignoreClick(now: 10, closedAt: 0))          // never closed
        expect(MenuPanel.ignoreClick(now: 10.1, closedAt: 10))        // the closing click itself
        expect(!MenuPanel.ignoreClick(now: 10.31, closedAt: 10))      // a new click
        expect(!MenuPanel.ignoreClick(now: 9, closedAt: 10))          // clock went back
    }

    func tickRing() {
        var r = TickRing()
        expect(r.samples.isEmpty)
        for i in 1...3 { r.push(Float(i)) }
        r.push(0); r.push(.nan)                                       // not a tick
        expect(r.samples == [1, 2, 3])
        for i in 4...300 { r.push(Float(i)) }
        expect(r.count == 256 && r.samples.first == 45 && r.samples.last == 300)
        r.clear()
        expect(r.samples.isEmpty)
    }
}
