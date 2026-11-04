// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
import AppKit

// macOS's own Zoom drives the same WindowServer zoom as Wind. When one of its shortcuts fires while
// Wind is zoomed (most often Ctrl+scroll, which macOS handles before Wind's event tap can swallow
// it), the two fight over the pointer and panning breaks until macOS Zoom is toggled once (issue
// #1). Wind cannot write these settings (the domain is protected), so it detects them and offers
// to switch them off by pressing the real switches in System Settings.

enum SystemZoom {
    private static let domain = "com.apple.universalaccess" as CFString

    private static func bool(_ key: String, _ domain: CFString = domain) -> Bool? {
        CFPreferencesAppSynchronize(domain)
        guard let v = CFPreferencesCopyAppValue(key as CFString, domain) else { return nil }
        return (v as? NSNumber)?.boolValue
    }

    /// The macOS Zoom shortcuts that are switched on, as their Settings labels.
    static func shortcutsOn() -> [String] {
        var on: [String] = []
        if bool("closeViewHotkeysEnabled") == true { on.append(keyboardLabel) }
        if bool("closeViewScrollWheelToggle") == true { on.append(scrollLabel) }
        if bool("closeViewTrackpadGestureZoomEnabled") == true { on.append(trackpadLabel) }
        return on
    }

    /// macOS Zoom is zoomed in right now.
    static var engaged: Bool { bool("closeViewZoomedIn") == true }

    static let keyboardLabel = "Use keyboard shortcuts to zoom"
    static let trackpadLabel = "Use trackpad gesture to zoom"
    static let scrollLabel = "Use scroll gesture with modifier keys to zoom"

    /// Release an engaged macOS Zoom: its own toggle shortcut (Option-Command-8) works when posted
    /// by Wind, but only while "Use keyboard shortcuts to zoom" is on (measured 2026-10-29).
    /// A 1x zoom write clears the screen but leaves macOS Zoom believing it is zoomed in.
    static func releaseEngaged(completion: @escaping () -> Void) {
        guard engaged else { completion(); return }
        // Only the default toggle (Option-Command-8, symbolic hotkey 15) is sent; a remapped or
        // disabled one would go to the frontmost app instead.
        guard toggleShortcutIsDefault else {
            Log.error("macOS Zoom is zoomed in but its toggle shortcut is remapped or off; release it with Ctrl+scroll down")
            completion(); return
        }
        let press = {
            pressToggle()
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.8) { completion() }
        }
        if bool("closeViewHotkeysEnabled") == true { press(); return }
        SettingsToggle.set(pane: SettingsToggle.zoomPane, label: keyboardLabel, on: true) { r in
            if case .notFound = r { completion(); return }        // never send it into another app
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { press() }
        }
    }

    private static func pressToggle() {
        let src = CGEventSource(stateID: .hidSystemState)
        for down in [true, false] {
            let e = CGEvent(keyboardEventSource: src, virtualKey: 0x1C, keyDown: down)   // the 8 key
            e?.flags = [.maskAlternate, .maskCommand]
            e?.post(tap: .cghidEventTap)
        }
        Log.info("macOS Zoom was zoomed in: sent its toggle shortcut")
    }

    /// Release an engaged macOS Zoom right away when that needs no trip through System Settings
    /// (its keyboard shortcuts are on with the default toggle). Wind calls this as it starts to
    /// zoom in, so macOS Zoom does not magnify Wind's view a second time. False: not released.
    static func releaseQuickly() -> Bool {
        guard engaged, bool("closeViewHotkeysEnabled") == true, toggleShortcutIsDefault else { return false }
        pressToggle()
        return true
    }

    /// Symbolic hotkey 15 (Zoom on/off) keeps its default key, Option-Command-8. Whether it is
    /// enabled follows the "Use keyboard shortcuts to zoom" switch, which the release turns on.
    static var toggleShortcutIsDefault: Bool {
        guard let all = UserDefaults(suiteName: "com.apple.symbolichotkeys")?.dictionary(forKey: "AppleSymbolicHotKeys"),
              let hk = all["15"] as? [String: Any] else { return true }
        guard let value = hk["value"] as? [String: Any], let p = value["parameters"] as? [Int], p.count == 3 else { return true }
        return p[1] == 28 && p[2] == 0x180000                  // the 8 key (kVK_ANSI_8), Option+Command
    }

    private static var inFlight = false

    /// Release macOS Zoom if it is zoomed in, then switch every macOS Zoom shortcut off, one
    /// switch at a time, and close System Settings if Wind opened it.
    static func turnOffShortcuts(completion: @escaping (Bool) -> Void) {
        guard !inFlight else { return }                         // a second click while one runs
        inFlight = true
        let wasRunning = !NSRunningApplication.runningApplications(withBundleIdentifier: "com.apple.systempreferences").isEmpty
        releaseEngaged {
            turnOffSwitches(wasRunning: wasRunning) { ok in inFlight = false; completion(ok) }
        }
    }

    /// `retries`: System Settings sometimes never shows a switch within the wait (2 of 5 e2e runs,
    /// 2026-11-01), so a pass that missed one runs once more.
    private static func turnOffSwitches(wasRunning: Bool, retries: Int = 1, completion: @escaping (Bool) -> Void) {
        let labels = [keyboardLabel, trackpadLabel, scrollLabel]
        var ok = true
        func next(_ i: Int) {
            guard i < labels.count else {
                if !ok && retries > 0 && !shortcutsOn().isEmpty {
                    Log.info("macOS Zoom: a switch was not found, trying once more")
                    turnOffSwitches(wasRunning: wasRunning, retries: retries - 1, completion: completion)
                    return
                }
                if !wasRunning { closeSettings() }
                // System Settings writes the preferences a moment after the press.
                func verify(_ tries: Int) {
                    if (shortcutsOn().isEmpty && !engaged) || tries == 0 { completion(ok && shortcutsOn().isEmpty && !engaged); return }
                    DispatchQueue.main.asyncAfter(deadline: .now() + 0.25) { verify(tries - 1) }
                }
                verify(8)
                return
            }
            SettingsToggle.set(pane: SettingsToggle.zoomPane, label: labels[i], on: false) { r in
                if case .notFound(let why) = r { ok = false; Log.error("macOS Zoom: \(labels[i]): \(why)") }
                next(i + 1)
            }
        }
        next(0)
    }

    private static func closeSettings() {
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.4) {
            NSRunningApplication.runningApplications(withBundleIdentifier: "com.apple.systempreferences").first?.terminate()
        }
    }
}
