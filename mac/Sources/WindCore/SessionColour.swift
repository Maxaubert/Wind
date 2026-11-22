// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
import Foundation

/// The screen colour (Warmth, Brightness) is session-only, as on Windows (#601, `kSessionKeys` and
/// `ResetSessionKeys` in src/config/profiles.h): it applies at once but goes back to its defaults at
/// every plain Wind start. A relaunch Wind caused itself keeps it: `relaunchArgument` on the command
/// line, or a marker the quitting Wind left (`keepsColour`). AppDelegate resets config.json before the
/// first config load, so the colour filter is never applied with the old values.
public enum SessionColour {
    /// config.json keys reset at a plain start: Windows' colorWarmPct, colorDimPct, colorSatPct. A key
    /// Config does not have yet (satPct until Saturation is ported, #599) has no default and is skipped.
    public static let keys = ["warmthPct", "dimPct", "satPct"]
    /// A launch argument for a relaunch Wind starts itself: the colour is kept.
    public static let relaunchArgument = "--keep-colour"
    /// A quit marker older than this (seconds) is a plain start: macOS's "Quit & Reopen" relaunches
    /// within a second or two, a person starting Wind again later gets the defaults.
    public static let markerWindow: TimeInterval = 60
    /// A permission asked for this long before quitting (seconds) makes the quit a likely
    /// "Quit & Reopen" after a grant (Screen Recording asks for it), so the quit leaves the marker.
    public static let permissionWindow: TimeInterval = 15 * 60

    /// Whether this launch keeps the colour config.json has.
    public static func keepsColour(arguments: [String], quitMarker: Date?, now: Date) -> Bool {
        if arguments.contains(relaunchArgument) { return true }
        guard let m = quitMarker else { return false }
        let age = now.timeIntervalSince(m)
        return age >= 0 && age <= markerWindow
    }

    /// Whether a quit now should leave the keep marker: a permission was asked for shortly before.
    public static func quitKeepsColour(permissionAskedAt: Date?, now: Date) -> Bool {
        guard let p = permissionAskedAt else { return false }
        let age = now.timeIntervalSince(p)
        return age >= 0 && age <= permissionWindow
    }

    /// config.json with the colour keys at their defaults, or nil when nothing changes (already the
    /// defaults, absent, or the file is not a JSON object: an unreadable file is left alone, its other
    /// keys untouched). Only the colour keys change; every other key keeps its value.
    public static func reset(_ data: Data) -> (data: Data, was: [String: Any])? {
        guard let obj = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else { return nil }
        let defaults = (try? JSONSerialization.jsonObject(with: JSONEncoder().encode(Config()))) as? [String: Any] ?? [:]
        var out = obj
        var was: [String: Any] = [:]
        for k in keys {
            guard let v = obj[k], let def = defaults[k] as? Double else { continue }
            if let n = v as? NSNumber, CFGetTypeID(n) != CFBooleanGetTypeID(), n.doubleValue == def { continue }
            was[k] = v
            out[k] = def
        }
        if was.isEmpty { return nil }
        guard let d = try? JSONSerialization.data(withJSONObject: out, options: [.prettyPrinted, .sortedKeys]) else { return nil }
        return (d, was)
    }
}
