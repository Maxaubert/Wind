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
    static let shakeLabel = "Shake mouse pointer to locate"

    /// Shake-to-locate enlarges the pointer on fast movement, which zoomed panning triggers a lot.
    static var shakeToLocateOn: Bool {
        bool("CGDisableCursorLocationMagnification", kCFPreferencesAnyApplication) != true
    }

    /// Switch every macOS Zoom shortcut off, one switch at a time, then close System Settings
    /// if Wind opened it.
    static func turnOffShortcuts(completion: @escaping (Bool) -> Void) {
        let wasRunning = !NSRunningApplication.runningApplications(withBundleIdentifier: "com.apple.systempreferences").isEmpty
        let labels = [keyboardLabel, trackpadLabel, scrollLabel]
        var ok = true
        func next(_ i: Int) {
            guard i < labels.count else {
                if !wasRunning { closeSettings() }
                // System Settings writes the preferences a moment after the press.
                func verify(_ tries: Int) {
                    if shortcutsOn().isEmpty || tries == 0 { completion(ok && shortcutsOn().isEmpty); return }
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

    static func setShakeToLocate(_ on: Bool, completion: @escaping (Bool) -> Void) {
        let wasRunning = !NSRunningApplication.runningApplications(withBundleIdentifier: "com.apple.systempreferences").isEmpty
        SettingsToggle.set(pane: SettingsToggle.displayPane, label: shakeLabel, on: on) { r in
            if !wasRunning { closeSettings() }
            if case .notFound = r { completion(false) } else { completion(true) }
        }
    }

    private static func closeSettings() {
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.4) {
            NSRunningApplication.runningApplications(withBundleIdentifier: "com.apple.systempreferences").first?.terminate()
        }
    }
}
