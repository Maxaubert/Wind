import Foundation

@testable import WindCore

/// The cases Wind for Mac shares with Windows Wind (the repo's tests/fixtures/shared/, also read by
/// tests/test_shared_fixtures.cpp): view geometry, the zoom controller and the view owner, which are
/// ports of the Windows code. Rows tagged all or mac run here; win rows are Windows-only. The file
/// headers give the formats. Windows is the origin, so a change there regenerates the all rows and
/// this side has to follow.
struct SharedFixtureTests {
    /// mac/Tests/WindCoreChecks/ -> the repo root.
    static let dir = URL(fileURLWithPath: #filePath).deletingLastPathComponent().deletingLastPathComponent()
        .deletingLastPathComponent().deletingLastPathComponent().appendingPathComponent("tests/fixtures/shared")
    static let tolerance = 1e-6   // documented in the fixture headers

    struct BadRow: Error, CustomStringConvertible {
        let description: String
        init(_ d: String) { description = d }
    }

    func viewMathCases() throws { try runFile("view_math_cases.txt", atLeast: 50) }
    func zoomControllerCases() throws { try runFile("zoom_controller_cases.txt", atLeast: 50) }
    func viewOwnerCases() throws { try runFile("view_owner_cases.txt", atLeast: 14) }

    func runFile(_ name: String, atLeast: Int) throws {
        let text = try String(contentsOf: Self.dir.appendingPathComponent(name), encoding: .utf8)
        var n = 0
        for raw in text.split(whereSeparator: \.isNewline) {
            let line = raw.trimmingCharacters(in: .whitespaces)
            if line.isEmpty || line.hasPrefix("#") { continue }
            let toks = line.split(whereSeparator: { $0 == " " || $0 == "\t" }).map(String.init)
            guard toks.count >= 2, ["all", "win", "mac"].contains(toks[1]) else {
                print("  \(name): bad row \"\(line)\""); expect(false); continue
            }
            if toks[1] == "win" { continue }
            let body = Array(toks.dropFirst(2))
            let want = body.filter { $0.hasPrefix("=") }.map { String($0.dropFirst()) }
            let got: [String]
            do {
                switch toks[0] {
                case "zoom": got = try zoomSequence(body)
                case "owner": got = try ownerSequence(body)
                default: got = try plain(toks[0], Array(body.prefix(while: { !$0.hasPrefix("=") })))
                }
            } catch {
                print("  \(name): \"\(line)\": \(error)"); expect(false); continue
            }
            let ok = got.count == want.count && zip(want, got).allSatisfy { Self.same($0.0, $0.1) }
            if !ok { print("  \(name): \"\(line)\"\n    got =\(got.joined(separator: " ="))") }
            expect(ok)
            n += 1
        }
        expect(n >= atLeast)
    }

    static func same(_ want: String, _ got: String) -> Bool {
        if let w = Double(want), let g = Double(got) { return abs(w - g) <= tolerance }
        return want == got
    }

    static func num(_ v: Double) -> String { String(format: "%.10g", v) }

    func number(_ s: String) throws -> Double {
        guard let v = Double(s) else { throw BadRow("not a number: \(s)") }
        return v
    }

    func whole(_ s: String) throws -> Int {
        let v = try number(s)
        guard v == v.rounded() else { throw BadRow("not a whole number: \(s)") }
        return Int(v)
    }

    func keyValue(_ t: String) -> (String, String) {
        guard let eq = t.firstIndex(of: "=") else { return (t, "") }
        return (String(t[..<eq]), String(t[t.index(after: eq)...]))
    }

    /// One call of a plain function: every input comes before the expected values.
    func plain(_ fn: String, _ args: [String]) throws -> [String] {
        let a = try args.map { try number($0) }
        func need(_ n: Int) throws { if a.count != n { throw BadRow("\(fn): wrong input count") } }
        let num = Self.num
        switch fn {
        case "clamp":
            try need(5)
            let c = clampCenter(Pt(a[0], a[1]), level: a[2], screen: Sz(a[3], a[4]))
            return [num(c.x), num(c.y)]
        case "edge":
            try need(12)
            let c = edgePanCenter(cur: Pt(a[0], a[1]), ptr: Pt(a[2], a[3]), level: a[4], screen: Sz(a[5], a[6]),
                                  marginPct: a[7], body: CursorBody(l: a[8], t: a[9], r: a[10], b: a[11]))
            return [num(c.x), num(c.y)]
        case "track":
            try need(11)
            guard let c = trackTargetCenter(Rc(l: a[0], t: a[1], r: a[2], b: a[3]), cur: Pt(a[4], a[5]), level: a[6],
                                            screen: Sz(a[7], a[8]), align: Int(a[9]), marginPct: a[10]) else { return ["none"] }
            return [num(c.x), num(c.y)]
        case "still":
            try need(11)
            let c = stillViewTarget(Pt(a[0], a[1]), cur: Pt(a[2], a[3]), ptr: Pt(a[4], a[5]), level: a[6],
                                    screen: Sz(a[7], a[8]), marginPct: a[9], leadPct: a[10])
            return [num(c.x), num(c.y)]
        case "keypan":
            try need(3)
            return [num(keyPanSpeed(level: a[0], screenWidth: a[1], panSpeed: a[2]))]
        case "dir":
            try need(2)
            switch resolveDirection(inHeld: a[0] != 0, outHeld: a[1] != 0) {
            case .zoomIn: return ["in"]
            case .zoomOut: return ["out"]
            case .none: return ["none"]
            }
        case "notch":
            try need(1)
            return [num(wheelNotchStep(speed: a[0]))]
        case "quick":
            try need(4)
            let r = applyQuickZoom(cur: a[0], stored: a[1], def: a[2], maxLevel: a[3])
            return [num(r.newLevel), num(r.newStored)]
        default:
            throw BadRow("unknown function \(fn)")
        }
    }

    /// A zoom step sequence: one output per '=' token, in order.
    func zoomSequence(_ toks: [String]) throws -> [String] {
        var minL = 1.0, maxL = 30.0
        for t in toks {
            let (k, v) = keyValue(t)
            if k == "min" { minL = try number(v) }
            if k == "max" { maxL = try number(v) }
        }
        var z = ZoomController(minLevel: minL, maxLevel: maxL)
        var out: [String] = []
        for t in toks {
            if t.hasPrefix("=") { out.append(Self.num(z.level)); continue }
            let (k, v) = keyValue(t)
            switch k {
            case "min", "max": break
            case "profile":
                let p = try v.split(separator: ",").map { try number(String($0)) }
                guard p.count == 5 else { throw BadRow("profile needs 5 values") }
                z.setProfile(inSpeed: p[0], outSpeed: p[1], smooth: p[2] != 0, accel: p[3], rampSeconds: p[4])
            case "ease": try z.setEaseOut(seconds: number(v))
            case "in": z.direction = .zoomIn
            case "out": z.direction = .zoomOut
            case "none": z.direction = .none
            case "t":
                let p = v.split(separator: "*").map(String.init)
                guard p.count == 2 else { throw BadRow("t needs HZ*N") }
                let dt = try 1.0 / number(p[0])
                let n = try whole(p[1])
                for _ in 0..<n { z.tick(dt) }
            case "wheel": try z.wheelNotches(Double(whole(v)))
            case "step":
                let p = v.split(separator: ",").map(String.init)
                guard p.count == 2 else { throw BadRow("step needs N,S") }
                try z.stepTarget(Double(whole(p[0])), step: number(p[1]))
            case "set": try z.setLevel(number(v))
            case "reset": z.reset()
            default: throw BadRow("unknown zoom token \(t)")
            }
        }
        return out
    }

    /// A view owner step sequence: each '=' token runs one stepViewOwner.
    func ownerSequence(_ toks: [String]) throws -> [String] {
        var s = ViewOwnerState()
        var i = ViewOwnerInputs()
        i.enabled = true; i.trackCaret = true; i.trackFocus = true; i.dtMs = 8
        var out: [String] = []
        for t in toks {
            if t.hasPrefix("=") {
                stepViewOwner(&s, i)
                let owner: String
                switch s.owner {
                case .mouse: owner = "mouse"
                case .caret: owner = "caret"
                case .focus: owner = "focus"
                case .keys: owner = "keys"
                }
                out.append("\(owner):\(s.warpPointer ? 1 : 0):\(s.target.seq)")
                i.mouseDx = 0; i.mouseDy = 0; i.buttonDown = false   // these last one step
                continue
            }
            if t == "reset" { resetViewOwnerForSession(&s); continue }
            let (k, v) = keyValue(t)
            switch k {
            case "en": i.enabled = try whole(v) != 0
            case "tc": i.trackCaret = try whole(v) != 0
            case "tf": i.trackFocus = try whole(v) != 0
            case "dx": i.mouseDx = try number(v)
            case "dy": i.mouseDy = try number(v)
            case "btn": i.buttonDown = try whole(v) != 0
            case "sb": i.msSinceButton = try number(v)
            case "sk": i.msSinceKey = try number(v)
            case "kab": i.keyAfterButton = try whole(v) != 0
            case "dt": i.dtMs = try number(v)
            case "seq": i.snap.seq = try UInt(whole(v))
            case "pan": i.panning = try whole(v) != 0
            case "act": i.trackActive = try whole(v) != 0
            case "kind":
                switch v {
                case "none": i.snap.kind = .none
                case "caret": i.snap.kind = .caret
                case "focus": i.snap.kind = .focus
                default: throw BadRow("unknown kind \(v)")
                }
            default: throw BadRow("unknown owner token \(t)")
            }
        }
        return out
    }
}
