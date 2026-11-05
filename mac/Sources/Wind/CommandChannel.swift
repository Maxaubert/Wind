// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
#if WIND_TEST_CHANNEL
import AppKit
import WindCore

// Commands written to ~/Library/Application Support/Wind/command (one per line) by scripts/windctl.
// Compiled only into test builds (scripts/build.sh test); the installed app has no channel.
// Used by the end-to-end tests: Wind has the Accessibility grant, so the synthetic input it posts
// goes through the real event tap exactly like hardware input.
//
//   level <x>                  snap to a level        glide <x>     glide to a level
//   hold <action> <ms>         hold an action         press|release <action>
//   post key <bind> down|up    synthetic key event    post mouse <n> down|up
//   post move <dx> <dy>        synthetic mouse move   post scroll <lines> [mods]
//   warp <x> <y>               move the pointer       lock on|off   force the game-lock regime
//   settoggle on|off <label>   a macOS Zoom shortcut switch (exact label)
//   fixzoom                    switch every macOS Zoom shortcut off
//   menu <secs>                open Wind's own menu-bar menu; it closes itself after secs (no command
//                              is read while it tracks)
//   settings [onboard|close]   open the settings window (or the first-run setup), or close it
//   settings offscreen on|off  later opens place the window off every display, without focus
//   settings js <base64>       run the decoded JS (an async function body) in the settings page;
//                              the result goes to js.json (scripts/e2e_settings.py)
//   state                      just write state.json  quit

enum CommandChannel {
    static func run(_ line: String, _ app: AppDelegate) {
        let w = line.split(separator: " ").map(String.init)
        guard let cmd = w.first else { return }
        Log.info("command: \(line)")
        let mag = app.mag
        switch cmd {
        case "level": if w.count > 1, let v = Double(w[1]) { mag.setLevel(v, glide: false) }
        case "glide": if w.count > 1, let v = Double(w[1]) { mag.setLevel(v, glide: true) }
        case "hold":
            if w.count > 2, let a = Action(rawValue: w[1]), let ms = Double(w[2]) {
                mag.setHeld(a, true)
                DispatchQueue.main.asyncAfter(deadline: .now() + ms / 1000) { mag.setHeld(a, false) }
            }
        case "press": if w.count > 1, let a = Action(rawValue: w[1]) { mag.setHeld(a, true) }
        case "release": if w.count > 1, let a = Action(rawValue: w[1]) { mag.setHeld(a, false) }
        case "post": post(Array(w.dropFirst()))
        case "warp":
            if w.count > 2, let x = Double(w[1]), let y = Double(w[2]) {
                CGWarpMouseCursorPosition(CGPoint(x: x, y: y))
                CGAssociateMouseAndMouseCursorPosition(1)
            }
        case "lock": mag.forceLock(w.count > 1 && w[1] == "on")
        case "cursor": SkyLight.setCursorHidden(w.count > 1 && w[1] == "hide")   // like macOS hiding it after typing
        case "wcur":                                    // wcur sync|pre on|off: Wind's cursor timing (A/B tests)
            if w.count > 2 {
                if w[1] == "sync" { WindCursor.syncCommit = w[2] == "on" }
                if w[1] == "pre" { WindCursor.preCommit = w[2] == "on" }
                if w[1] == "delay" { Magnifier.tickDelay = Double(w[2]).map { $0 / 1000 } }   // "default": the engine's
            }
        case "mods":mag.testMods = w.count > 1 ? parseBind(w[1])?.mods : nil   // mods ctrl | mods (clear)
        case "settoggle":
            // settoggle on|off <label>: only the switches Wind itself manages (security review:
            // the channel must not become a general System Settings remote).
            let allowed = [SystemZoom.keyboardLabel, SystemZoom.trackpadLabel, SystemZoom.scrollLabel]
            if w.count > 2, let label = allowed.first(where: { $0.lowercased() == w[2...].joined(separator: " ").lowercased() }) {
                SettingsToggle.set(pane: SettingsToggle.zoomPane, label: label, on: w[1] == "on") { r in Log.info("settoggle result: \(r)") }
            }
        case "settings": app.testSettings(Array(w.dropFirst()))   // settings [onboard|close] | settings js <base64>
        case "menu": app.testMenu(w.count > 1 ? Double(w[1]) ?? 2 : 2)   // menu <secs>: Wind's own menu-bar menu, closed after secs
        case "login": app.perform(Selector(("toggleLogin")))
        case "fixzoom": app.perform(Selector(("turnOffSystemZoom")))
        case "pressbutton":
            // pressbutton <bundle id> <button title>: dismiss a dialog a test left behind.
            if w.count > 2, let app = NSRunningApplication.runningApplications(withBundleIdentifier: w[1]).first {
                let title = w[2...].joined(separator: " ")
                let root = AXUIElementCreateApplication(app.processIdentifier)
                AXUIElementSetMessagingTimeout(root, 0.5)
                func find(_ el: AXUIElement, _ d: Int) -> AXUIElement? {
                    if d > 12 { return nil }
                    var r: CFTypeRef?, t: CFTypeRef?, k: CFTypeRef?
                    AXUIElementCopyAttributeValue(el, kAXRoleAttribute as CFString, &r)
                    AXUIElementCopyAttributeValue(el, kAXTitleAttribute as CFString, &t)
                    if (r as? String) == "AXButton", (t as? String) == title { return el }
                    AXUIElementCopyAttributeValue(el, kAXChildrenAttribute as CFString, &k)
                    for c in (k as? [AXUIElement]) ?? [] { if let f = find(c, d + 1) { return f } }
                    return nil
                }
                if let b = find(root, 0) { Log.info("press \(title): \(AXUIElementPerformAction(b, kAXPressAction as CFString).rawValue)") }
                else { Log.info("press \(title): not found") }
            }
        case "quit": NSApp.terminate(nil)
        default: break
        }
    }

    private static let source = CGEventSource(stateID: .hidSystemState)

    private static func post(_ w: [String]) {
        guard let kind = w.first else { return }
        switch kind {
        case "key":
            guard w.count > 2, let b = parseBind(w[1]), case .key(let code) = b.trigger else { return }
            let e = CGEvent(keyboardEventSource: source, virtualKey: code, keyDown: w[2] == "down")
            e?.flags = CGEventFlags(rawValue: b.mods.rawValue)
            e?.post(tap: .cghidEventTap)
        case "mouse":
            guard w.count > 2, let n = Int(w[1]) else { return }
            let p = Magnifier.pointer() ?? .zero
            let e = CGEvent(mouseEventSource: source, mouseType: w[2] == "down" ? .otherMouseDown : .otherMouseUp,
                            mouseCursorPosition: p, mouseButton: CGMouseButton(rawValue: UInt32(n)) ?? .center)
            e?.setIntegerValueField(.mouseEventButtonNumber, value: Int64(n))
            e?.post(tap: .cghidEventTap)
        case "move":
            guard w.count > 2, let dx = Double(w[1]), let dy = Double(w[2]) else { return }
            let p = Magnifier.pointer() ?? .zero
            let e = CGEvent(mouseEventSource: source, mouseType: .mouseMoved,
                            mouseCursorPosition: CGPoint(x: p.x + dx, y: p.y + dy), mouseButton: .left)
            e?.setIntegerValueField(.mouseEventDeltaX, value: Int64(dx))
            e?.setIntegerValueField(.mouseEventDeltaY, value: Int64(dy))
            e?.post(tap: .cghidEventTap)
        case "scroll":
            guard w.count > 1, let lines = Int32(w[1]) else { return }
            let e = CGEvent(scrollWheelEvent2Source: source, units: .line, wheelCount: 1, wheel1: lines, wheel2: 0, wheel3: 0)
            if w.count > 2, let b = parseBind(w[2]) { e?.flags = CGEventFlags(rawValue: b.mods.rawValue) }
            e?.post(tap: .cghidEventTap)
        default: break
        }
    }
}
#endif
