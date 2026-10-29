import Foundation

// A bind is 0-4 modifiers plus one key or mouse button, written as text in the config:
//   "ctrl+opt+equal", "mouse4", "shift+mouse3", "f13", "ctrl" (modifier-only, wheel zoom).
// Modifier bit values match CGEventFlags so the app can compare them directly.

public struct Mods: OptionSet, Hashable {
    public let rawValue: UInt64
    public init(rawValue: UInt64) { self.rawValue = rawValue }
    public static let shift = Mods(rawValue: 0x0002_0000)
    public static let ctrl  = Mods(rawValue: 0x0004_0000)
    public static let opt   = Mods(rawValue: 0x0008_0000)
    public static let cmd   = Mods(rawValue: 0x0010_0000)
    public static let all: Mods = [.shift, .ctrl, .opt, .cmd]
}

public enum Trigger: Hashable {
    case key(UInt16)        // macOS virtual key code (kVK_*)
    case mouse(Int)         // CGEvent button number: 2 = middle, 3 = back, 4 = forward, ...
    case none               // modifier-only (wheel zoom modifier)
}

public struct Bind: Hashable, CustomStringConvertible {
    public var mods: Mods
    public var trigger: Trigger
    public init(mods: Mods, trigger: Trigger) { self.mods = mods; self.trigger = trigger }

    public var description: String { formatBind(self) }
}

/// kVK_* codes for the names a config may use.
public let keyCodes: [String: UInt16] = {
    var m: [String: UInt16] = [
        "a": 0x00, "s": 0x01, "d": 0x02, "f": 0x03, "h": 0x04, "g": 0x05, "z": 0x06, "x": 0x07,
        "c": 0x08, "v": 0x09, "b": 0x0B, "q": 0x0C, "w": 0x0D, "e": 0x0E, "r": 0x0F, "y": 0x10,
        "t": 0x11, "1": 0x12, "2": 0x13, "3": 0x14, "4": 0x15, "6": 0x16, "5": 0x17, "equal": 0x18,
        "9": 0x19, "7": 0x1A, "minus": 0x1B, "8": 0x1C, "0": 0x1D, "rightbracket": 0x1E, "o": 0x1F,
        "u": 0x20, "leftbracket": 0x21, "i": 0x22, "p": 0x23, "return": 0x24, "l": 0x25, "j": 0x26,
        "quote": 0x27, "k": 0x28, "semicolon": 0x29, "backslash": 0x2A, "comma": 0x2B, "slash": 0x2C,
        "n": 0x2D, "m": 0x2E, "period": 0x2F, "tab": 0x30, "space": 0x31, "grave": 0x32,
        "delete": 0x33, "escape": 0x35, "capslock": 0x39,
        "f17": 0x40, "keypaddecimal": 0x41, "keypadmultiply": 0x43, "keypadplus": 0x45,
        "keypadclear": 0x47, "keypaddivide": 0x4B, "keypadenter": 0x4C, "keypadminus": 0x4E,
        "f18": 0x4F, "f19": 0x50, "keypadequals": 0x51, "keypad0": 0x52, "keypad1": 0x53,
        "keypad2": 0x54, "keypad3": 0x55, "keypad4": 0x56, "keypad5": 0x57, "keypad6": 0x58,
        "keypad7": 0x59, "f20": 0x5A, "keypad8": 0x5B, "keypad9": 0x5C,
        "f5": 0x60, "f6": 0x61, "f7": 0x62, "f3": 0x63, "f8": 0x64, "f9": 0x65, "f11": 0x67,
        "f13": 0x69, "f16": 0x6A, "f14": 0x6B, "f10": 0x6D, "f12": 0x6F, "f15": 0x71,
        "help": 0x72, "home": 0x73, "pageup": 0x74, "forwarddelete": 0x75, "f4": 0x76, "end": 0x77,
        "f2": 0x78, "pagedown": 0x79, "f1": 0x7A, "left": 0x7B, "right": 0x7C, "down": 0x7D, "up": 0x7E,
    ]
    m["esc"] = 0x35; m["plus"] = 0x18; m["="] = 0x18; m["-"] = 0x1B
    return m
}()

/// Canonical name per key code (the aliases are parse-only).
let keyNames: [UInt16: String] = {
    var m: [UInt16: String] = [:]
    for (name, code) in keyCodes where !["esc", "plus", "=", "-"].contains(name) { m[code] = name }
    return m
}()

let modNames: [String: Mods] = [
    "shift": .shift, "ctrl": .ctrl, "control": .ctrl, "opt": .opt, "option": .opt, "alt": .opt,
    "cmd": .cmd, "command": .cmd,
]

/// The key codes of the modifier keys themselves (they arrive as flagsChanged, never keyDown).
public let modifierKeyCodes: Set<UInt16> = [0x36, 0x37, 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F]

public func parseBind(_ text: String) -> Bind? {
    let parts = text.lowercased().split(separator: "+").map { $0.trimmingCharacters(in: .whitespaces) }
    if parts.isEmpty || parts.contains(where: { $0.isEmpty }) {
        // "ctrl++" style: the key itself is "+"
        if text.hasSuffix("++") { return parseBind(String(text.dropLast(2)) + "+plus") }
        return nil
    }
    var mods: Mods = []
    var trigger: Trigger = .none
    for (i, p) in parts.enumerated() {
        if let m = modNames[p] { mods.insert(m); continue }
        guard i == parts.count - 1, trigger == .none else { return nil }
        if p.hasPrefix("mouse"), let n = Int(p.dropFirst(5)), n >= 2, n <= 31 { trigger = .mouse(n) }
        else if let k = keyCodes[p] { trigger = .key(k) }
        else { return nil }
    }
    if trigger == .none && mods.isEmpty { return nil }
    return Bind(mods: mods, trigger: trigger)
}

public func formatBind(_ b: Bind) -> String {
    var parts: [String] = []
    if b.mods.contains(.ctrl) { parts.append("ctrl") }
    if b.mods.contains(.opt) { parts.append("opt") }
    if b.mods.contains(.shift) { parts.append("shift") }
    if b.mods.contains(.cmd) { parts.append("cmd") }
    switch b.trigger {
    case .key(let k):
        parts.append(keyNames[k] ?? String(format: "key0x%02X", k))
    case .mouse(let n): parts.append("mouse\(n)")
    case .none: break
    }
    return parts.joined(separator: "+")
}

/// Wind's bind rules, adapted to macOS: refuse binds that would break typing or system shortcuts.
/// Returns nil when allowed, else the reason.
public func bindProblem(_ b: Bind) -> String? {
    switch b.trigger {
    case .mouse(let n):
        if n < 2 { return "the left and right buttons cannot be zoom binds" }
        return nil
    case .none:
        return b.mods.isEmpty ? "empty bind" : nil
    case .key(let k):
        if modifierKeyCodes.contains(k) { return "a modifier key alone cannot be a key bind" }
        let fKeys: Set<UInt16> = [0x7A, 0x78, 0x63, 0x76, 0x60, 0x61, 0x62, 0x64, 0x65, 0x6D, 0x67, 0x6F, 0x69, 0x6B, 0x71, 0x6A, 0x40, 0x4F, 0x50, 0x5A]
        if fKeys.contains(k) { return nil }
        let nonTyping = b.mods.contains(.ctrl) || b.mods.contains(.opt) || b.mods.contains(.cmd)
        if !nonTyping { return "a typing key needs ctrl, opt or cmd" }
        if b.mods == [.cmd] {
            return "cmd plus one key is taken by app shortcuts; add ctrl or opt"
        }
        return nil
    }
}

/// Does a set of currently held modifiers satisfy a bind? Exact match on ctrl/opt/cmd/shift.
public func modsMatch(_ held: Mods, _ want: Mods) -> Bool {
    held.intersection(.all) == want
}
