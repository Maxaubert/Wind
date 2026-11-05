// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
import Foundation

// Wind's own screenshot selection (WindShot): the pure parts.

/// One of macOS's screenshot-area shortcuts as the user has it set.
public struct ShotHotkey: Equatable {
    public var keyCode: UInt16
    public var mods: Mods
    public var clipboard: Bool           // "Copy picture of selected area to the clipboard"
    public init(keyCode: UInt16, mods: Mods, clipboard: Bool) { self.keyCode = keyCode; self.mods = mods; self.clipboard = clipboard }
}

/// The area shortcuts from com.apple.symbolichotkeys' AppleSymbolicHotKeys: 30 saves the selected area
/// (Cmd+Shift+4 by default), 31 copies it (Ctrl+Cmd+Shift+4). An entry that is missing has macOS's
/// default; one with enabled = false is off (Wind leaves it alone); a changed key is taken as set.
/// Entry: {enabled, value: {parameters: [character, keyCode, modifierFlags], type}}.
public func screenshotAreaHotkeys(_ table: [String: Any]?) -> [ShotHotkey] {
    let defaults: [(String, UInt16, Mods, Bool)] = [("30", 21, [.cmd, .shift], false), ("31", 21, [.cmd, .shift, .ctrl], true)]
    var out: [ShotHotkey] = []
    for (id, code, mods, clip) in defaults {
        guard let e = table?[id] as? [String: Any] else { out.append(ShotHotkey(keyCode: code, mods: mods, clipboard: clip)); continue }
        if let on = e["enabled"] as? Bool, !on { continue }
        if let on = e["enabled"] as? NSNumber, !on.boolValue { continue }
        let p = ((e["value"] as? [String: Any])?["parameters"] as? [Any])?.compactMap { ($0 as? NSNumber)?.int64Value } ?? []
        guard p.count >= 3, p[1] >= 0, p[1] < 0xFFFF else { out.append(ShotHotkey(keyCode: code, mods: mods, clipboard: clip)); continue }
        out.append(ShotHotkey(keyCode: UInt16(p[1]), mods: Mods(rawValue: UInt64(p[2])).intersection(.all), clipboard: clip))
    }
    return out
}

/// The desktop rectangle a selection saves, in whole points: each corner goes to the nearest point
/// boundary (screencapture -R rounds a fractional rectangle outward, a point too big). The box is drawn
/// at sub-point places, so the saved edges are at most half a point from the drawn ones.
/// nil when it is under a point wide or high (a click, not a drag), as macOS takes no shot then.
public func shotRect(anchor a: Pt, end b: Pt, screen: Sz) -> (x: Int, y: Int, w: Int, h: Int)? {
    func axis(_ p: Double, _ q: Double, _ size: Double) -> (Int, Int) {
        let lo = Int(min(max(min(p, q), 0), size).rounded()), hi = Int(min(max(max(p, q), 0), size).rounded())
        return (lo, hi - lo)
    }
    let (x, w) = axis(a.x, b.x, screen.w), (y, h) = axis(a.y, b.y, screen.h)
    return w >= 1 && h >= 1 ? (x, y, w, h) : nil
}
