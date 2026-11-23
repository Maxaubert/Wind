// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
import Foundation

/// The menu bar panel's quick-control lists: which sliders and toggles it shows and in what order, from
/// Config.menuSliders / menuSliderOrder / menuToggles / menuToggleOrder. The rules are Windows' tray rules
/// (src/tray/tray_items.cpp, ui-shared/src/tray/listModel.js), so the panel and Settings > Menu bar read
/// the keys the same way; Tests/fixtures/menu_layout_cases.json checks both sides.
public enum MenuLayout {
    public static let maxSliders = 4

    /// Every eligible item, in the default order (mac/ui/src/menubar/menuModel.js SLIDERS and TOGGLES).
    public static let sliders = ["warmthPct", "dimPct", "satPct", "maxLevel", "zoomInSpeed", "zoomOutSpeed",
                                 "panSpeed", "zoomEaseOutMs", "panSmoothMs"]
    /// `keepEdges` stands for mouseAlign and trackAlign together; `engine` is the engine picker.
    public static let toggles = ["trackCaret", "trackFocus", "keepEdges", "engine"]
    public static let defaultSliders = ["warmthPct", "dimPct"]
    public static let defaultToggles = toggles

    public enum Kind: String, Sendable { case sliders, toggles }

    public struct Item: Equatable, Sendable {
        public let key: String
        public let on: Bool
        public init(key: String, on: Bool) { self.key = key; self.on = on }
    }

    /// The list in display order with each item's on state. Order = the order list's known items, then
    /// the enabled list's known items not yet seen, then every remaining eligible item. Unknown names are
    /// dropped; sliders past the fourth enabled one read as off. Each entry may itself be a comma list
    /// (a hand-edited "a,b" string), split as the page writes it.
    public static func parse(_ kind: Kind, enabled: [String], order: [String]) -> [Item] {
        let eligible = kind == .sliders ? sliders : toggles
        let cap = kind == .sliders ? maxSliders : 0
        let en = split(enabled)
        var out: [String] = []
        for k in split(order) + en + eligible where eligible.contains(k) && !out.contains(k) { out.append(k) }
        var on = 0
        return out.map { k in
            let isOn = en.contains(k) && (cap == 0 || on < cap)
            if isOn { on += 1 }
            return Item(key: k, on: isOn)
        }
    }

    public static func sliders(_ c: Config) -> [Item] { parse(.sliders, enabled: c.menuSliders, order: c.menuSliderOrder) }
    public static func toggles(_ c: Config) -> [Item] { parse(.toggles, enabled: c.menuToggles, order: c.menuToggleOrder) }

    /// The items the panel shows, in order.
    public static func shown(_ items: [Item]) -> [String] { items.filter(\.on).map(\.key) }

    /// The two lists a change writes back: the enabled keys and the full order.
    public static func serialise(_ items: [Item]) -> (enabled: [String], order: [String]) {
        (items.filter(\.on).map(\.key), items.map(\.key))
    }

    static func split(_ list: [String]) -> [String] {
        list.flatMap { $0.split(separator: ",").map { $0.trimmingCharacters(in: .whitespaces) } }.filter { !$0.isEmpty }
    }
}
