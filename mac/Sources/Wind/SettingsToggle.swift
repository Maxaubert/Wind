// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
import AppKit
import ApplicationServices

// Flips a switch in System Settings through the Accessibility API. Some settings Wind depends on
// (macOS Zoom's shortcuts, shake-to-locate) live in preference domains other processes cannot
// write, and a `defaults write` would not reach the running WindowServer anyway; pressing the
// real switch does both. Wind holds the Accessibility grant, so it can press it.

enum SettingsToggle {
    static let zoomPane = "x-apple.systempreferences:com.apple.Accessibility-Settings.extension?Zoom"
    static let displayPane = "x-apple.systempreferences:com.apple.Accessibility-Settings.extension?Display"

    enum Result: Equatable { case changed, alreadySet, notFound(String) }

    /// Opens the pane, finds the switch whose label contains `label`, and sets it. Calls back on main.
    static func set(pane: String, label: String, on: Bool, completion: @escaping (Result) -> Void) {
        NSWorkspace.shared.open(URL(string: pane)!)
        attempt(label: label, on: on, tries: 20, completion: completion)
    }

    private static func attempt(label: String, on: Bool, tries: Int, completion: @escaping (Result) -> Void) {
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.3) {
            guard let app = NSRunningApplication.runningApplications(withBundleIdentifier: "com.apple.systempreferences").first else {
                if tries > 0 { attempt(label: label, on: on, tries: tries - 1, completion: completion) } else { completion(.notFound("System Settings not running")) }
                return
            }
            let root = AXUIElementCreateApplication(app.processIdentifier)
            AXUIElementSetMessagingTimeout(root, 0.5)
            guard let sw = findSwitch(in: root, label: label.lowercased()) else {
                if tries > 0 { attempt(label: label, on: on, tries: tries - 1, completion: completion) }
                else { completion(.notFound("no switch labelled \"\(label)\"")) }
                return
            }
            let current = (value(sw, kAXValueAttribute) as? NSNumber)?.boolValue
            if current == on { completion(.alreadySet); return }
            let err = AXUIElementPerformAction(sw, kAXPressAction as CFString)
            Log.info("settings: pressed \"\(label)\" -> \(on) (\(err.rawValue))")
            completion(err == .success ? .changed : .notFound("press failed \(err.rawValue)"))
        }
    }

    private static func value(_ el: AXUIElement, _ attr: String) -> AnyObject? {
        var v: CFTypeRef?
        return AXUIElementCopyAttributeValue(el, attr as CFString, &v) == .success ? v : nil
    }

    private static func children(_ el: AXUIElement) -> [AXUIElement] {
        (value(el, kAXChildrenAttribute) as? [AXUIElement]) ?? []
    }

    private static func text(_ el: AXUIElement) -> String {
        [kAXTitleAttribute, kAXDescriptionAttribute, kAXValueAttribute, "AXLabel", kAXHelpAttribute]
            .compactMap { value(el, $0) as? String }.joined(separator: " ").lowercased()
    }

    private static func isSwitch(_ el: AXUIElement) -> Bool {
        let role = value(el, kAXRoleAttribute) as? String ?? ""
        let sub = value(el, kAXSubroleAttribute) as? String ?? ""
        return role == kAXCheckBoxRole as String || sub == "AXSwitch" || role == "AXSwitch"
    }

    /// Depth-first: a switch labelled with the text itself, or a switch in the same row (group)
    /// as a static text carrying the label.
    private static func findSwitch(in el: AXUIElement, label: String, depth: Int = 0) -> AXUIElement? {
        if depth > 40 { return nil }
        let kids = children(el)
        if isSwitch(el) && text(el).contains(label) { return el }
        // Settings lays a pane out flat: each label is followed by its description lines and then
        // its own switch. Take the first switch after the label, skipping text, but stop at any
        // other control (a row with a pop-up instead of a switch must not borrow the next row's).
        if let i = kids.firstIndex(where: { !isSwitch($0) && text($0).contains(label) }) {
            // A row has a few description lines at most: never walk into the next rows.
            for k in kids[(i + 1)...].prefix(5) {
                if isSwitch(k) { return k }
                let role = value(k, kAXRoleAttribute) as? String ?? ""
                if role != (kAXStaticTextRole as String) && role != (kAXGroupRole as String) { break }
            }
        }
        for k in kids {
            if let f = findSwitch(in: k, label: label, depth: depth + 1) { return f }
        }
        return nil
    }
}
