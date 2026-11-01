import Foundation

// Settings, stored as JSON in ~/Library/Application Support/Wind/config.json. Every key is
// optional in the file: missing keys take the defaults below, unknown keys are ignored, and the
// file is reloaded live when it changes. Defaults mirror Wind's config.h where a key exists there.

public struct Config: Codable, Equatable {
    // Binds (see Binds.swift for the syntax). Each action takes a list.
    public var zoomIn = ["mouse4", "ctrl+opt+equal"]
    public var zoomOut = ["mouse3", "ctrl+opt+minus"]
    public var wheelZoomMods = "ctrl"            // modifier(s) + scroll wheel/trackpad zooms; "" = off
    public var wheelZoomSpeed = 0.6               // zoom per wheel notch, relative to the hold speed
    public var quickZoom = ["ctrl+opt+z"]
    public var panLeft = ["ctrl+opt+left"]
    public var panRight = ["ctrl+opt+right"]
    public var panUp = ["ctrl+opt+up"]
    public var panDown = ["ctrl+opt+down"]
    public var inspect = ["ctrl+opt+i"]
    public var hideCursor: [String] = []          // toggles hiding the pointer (Wind's hideCursor hotkey)
    public var quit = ["ctrl+opt+escape"]

    // Zoom feel.
    public var maxLevel = 30.0
    public var zoomInSpeed = 1.75
    public var zoomOutSpeed = 1.75
    public var smoothZoom = true
    public var smoothZoomAccel = 3.0
    public var smoothZoomRamp = 0.6
    public var zoomEaseOutMs = 115.0
    public var quickZoomDefault = 4.0
    public var smoothImages = false               // false = nearest (sharp pixels), true = smoothed
    public var engine = "draw"                    // "draw" = Wind captures and draws (sub-pixel, issue #8, the default); "zoom" = macOS's zoom

    // Mouse tracking.
    public var mouseAlign = 0                     // 0 = centred on the pointer, 1 = within the edges
    public var mouseMarginPct = 0.0
    public var lockTracking = false               // pan from raw deltas when a game locks the pointer (off and hidden until games are tested on the Mac)
    public var cursorSensitivity = 1.0            // delta gain while locked
    public var panSpeed = 1.0
    public var panSmoothMs = 0.0                  // pointer glide (off: the zoom moves in whole points anyway)
    public var slowPrecision = false              // very slow motion keeps macOS's fine zoomed steps (speed changes on turns)
    public var precisionBelow = 100.0             // hand speed (pt/s at 1x) below which it applies; full 1x speed at twice that
    public var panMaxLagPt = 24.0                 // most the view may trail the pointer, screen points
    public var cursor = "system"                 // "system" = the real cursor; "wind" = Wind draws it (steady on whole-point zoom)
    public var slowPanGlide = false               // experimental: WindowServer glides each whole-point step of slow pans (issue #13)

    // Caret and focus tracking.
    public var trackCaret = true
    public var trackFocus = false
    public var trackAlign = 0                     // 0 = centred, 1 = within the edges
    public var trackGlideMs = 200.0
    public var hidePointerWhileTracking = true    // hide the pointer while the caret owns the view
    public var trackMarginPct = 15.0

    // Colour.
    public var warmthPct = 0.0                    // 0 = off, 100 = strongest
    public var dimPct = 100.0                     // 100 = no dimming

    /// macOS slows the pointer by the zoom level (it moves in screen space). true = Wind's
    /// behaviour: the pointer covers the same desktop distance per hand movement at any zoom.
    public var keepPointerSpeed = true

    public var logLevel = 1                       // 0 = quiet, 1 = sessions, 2 = verbose, 3 = + every zoom write

    // Settings window (ui/). Read by the page only, except onboarded (AppDelegate.needsSetup).
    public var uiPalette = "grey"                 // the built-in theme: grey, ember, ocean or hicon
    public var showAdvanced = false               // show the advanced rows on every page
    public var onboarded = false                  // the first-run setup was finished, skipped or closed

    public init() {}

    /// Decode a possibly partial JSON object on top of the defaults.
    public static func load(from data: Data) throws -> Config {
        var dropped: [String] = []
        return try load(from: data, dropped: &dropped)
    }

    /// As load(from:), and keys whose value has the wrong type are dropped back to their default
    /// (named in `dropped`) instead of rejecting the whole file.
    public static func load(from data: Data, dropped: inout [String]) throws -> Config {
        let defaults = try JSONSerialization.jsonObject(with: JSONEncoder().encode(Config())) as! [String: Any]
        guard let user = try JSONSerialization.jsonObject(with: data) as? [String: Any] else {
            throw NSError(domain: "Wind", code: 1, userInfo: [NSLocalizedDescriptionKey: "config.json is not a JSON object"])
        }
        var merged = defaults
        for (k, v) in user.sorted(by: { $0.key < $1.key }) where defaults[k] != nil {
            // A single string where a list is expected is accepted as a one-item list.
            var trial = merged
            if defaults[k] is [Any], let s = v as? String { trial[k] = [s] } else { trial[k] = v }
            if (try? JSONDecoder().decode(Config.self, from: JSONSerialization.data(withJSONObject: trial))) != nil {
                merged = trial
            } else {
                dropped.append(k)
            }
        }
        var c = try JSONDecoder().decode(Config.self, from: JSONSerialization.data(withJSONObject: merged))
        c.clamp()
        return c
    }

    public func encoded() -> Data {
        let e = JSONEncoder()
        e.outputFormatting = [.prettyPrinted, .sortedKeys]
        return (try? e.encode(self)) ?? Data("{}".utf8)
    }

    public mutating func clamp() {
        maxLevel = min(64, max(1.5, maxLevel))
        zoomInSpeed = min(4, max(0.25, zoomInSpeed))
        zoomOutSpeed = min(4, max(0.25, zoomOutSpeed))
        wheelZoomSpeed = min(3, max(0.1, wheelZoomSpeed))
        smoothZoomAccel = min(10, max(1, smoothZoomAccel))
        smoothZoomRamp = min(3, max(0, smoothZoomRamp))
        zoomEaseOutMs = min(1000, max(0, zoomEaseOutMs))
        quickZoomDefault = min(maxLevel, max(1.1, quickZoomDefault))
        mouseAlign = mouseAlign == 1 ? 1 : 0
        mouseMarginPct = min(40, max(0, mouseMarginPct))
        cursorSensitivity = min(8, max(0.1, cursorSensitivity))
        panSpeed = min(4, max(0.25, panSpeed))
        panSmoothMs = min(500, max(0, panSmoothMs))
        precisionBelow = min(1000, max(10, precisionBelow))   // Magnifier divides by it
        panMaxLagPt = min(40, max(0, panMaxLagPt))
        trackAlign = trackAlign == 1 ? 1 : 0
        trackGlideMs = min(2000, max(0, trackGlideMs))
        trackMarginPct = min(40, max(0, trackMarginPct))
        warmthPct = min(100, max(0, warmthPct))
        dimPct = min(100, max(10, dimPct))
        logLevel = min(3, max(0, logLevel))
        if engine != "zoom" { engine = "draw" }
        if cursor != "wind" { cursor = "system" }
        if !Config.palettes.contains(uiPalette) { uiPalette = "grey" }
    }

    /// The settings window's built-in themes (ui/src/design/themes.js).
    public static let palettes = ["grey", "ember", "ocean", "hicon"]

    /// Parsed binds per action, with any problems found (invalid text or a refused bind).
    public func binds() -> (binds: [Action: [Bind]], problems: [String]) {
        var out: [Action: [Bind]] = [:]
        var problems: [String] = []
        let table: [(Action, [String])] = [
            (.zoomIn, zoomIn), (.zoomOut, zoomOut), (.quickZoom, quickZoom),
            (.panLeft, panLeft), (.panRight, panRight), (.panUp, panUp), (.panDown, panDown),
            (.inspect, inspect), (.hideCursor, hideCursor), (.quit, quit),
        ]
        for (action, texts) in table {
            for t in texts where !t.isEmpty {
                guard let b = parseBind(t), b.trigger != .none else { problems.append("\(action): cannot read \"\(t)\""); continue }
                if let why = bindProblem(b), action != .quit { problems.append("\(action): \"\(t)\" refused, \(why)"); continue }
                out[action, default: []].append(b)
            }
        }
        return (out, problems)
    }

    public var wheelMods: Mods? {
        if wheelZoomMods.isEmpty { return nil }
        guard let b = parseBind(wheelZoomMods), b.trigger == .none else { return nil }
        return b.mods
    }
}

public enum Action: String, CaseIterable {
    case zoomIn, zoomOut, quickZoom, panLeft, panRight, panUp, panDown, inspect, hideCursor, quit
}
