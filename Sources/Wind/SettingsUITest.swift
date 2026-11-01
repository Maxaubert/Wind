#if WIND_TEST_CHANNEL
import AppKit
import ApplicationServices

// Test-only: drives Wind's Settings window through the Accessibility API, the way a person would
// (switch tabs, press switches, move sliders), and records what happened to config.json.
// The driving runs in a separate process (`Wind --uitest-client <pid>`, which inherits Wind's
// Accessibility grant): calls from Wind into itself are answered by its own main run loop and
// could deadlock it, freezing the event tap with it (seen in testing).

enum SettingsUITest {
    /// In Wind: open the window and start the client process; the client writes uitest.txt.
    static func run(openSettings: () -> Void) {
        openSettings()
        let p = Process()
        p.executableURL = URL(fileURLWithPath: CommandLine.arguments[0])
        p.arguments = ["--uitest-client", String(getpid())]
        try? p.run()
    }

    /// In the client process: drive the Settings window of process `pid`.
    static func client(pid: pid_t) -> Never {
        Thread.sleep(forTimeInterval: 1.0)
        let lines = drive(pid: pid)
        try? lines.joined(separator: "\n").write(to: Paths.support.appendingPathComponent("uitest.txt"), atomically: true, encoding: .utf8)
        exit(0)
    }

    private static func drive(pid: pid_t) -> [String] {
            var log: [String] = []
            func note(_ s: String) { log.append(s) }
            let app = AXUIElementCreateApplication(pid)
            // Every element made from here on gets a short timeout (set on the system-wide
            // element, it is global): with the default 6 s, a busy main thread turned the tree
            // walk into minutes.
            AXUIElementSetMessagingTimeout(AXUIElementCreateSystemWide(), 1)
            AXUIElementSetMessagingTimeout(app, 1)
            guard let win = (attr(app, kAXWindowsAttribute) as? [AXUIElement])?.first(where: { (attr($0, kAXTitleAttribute) as? String) == "Wind Settings" }) else {
                return ["FAIL no Settings window"]
            }
            note("PASS window open")
            let tabs = all(win) { role($0) == "AXRadioButton" || role($0) == "AXTab" }
            let names = tabs.compactMap { (attr($0, kAXTitleAttribute) as? String) ?? (attr($0, kAXDescriptionAttribute) as? String) }
            note((names.count >= 5 ? "PASS" : "FAIL") + " tabs \(names)")

            // Mouse tab: flip "Keep pointer speed when zoomed" and back, watching config.json.
            if let mouse = tabs.first(where: { label($0).contains("Mouse") }) {
                AXUIElementPerformAction(mouse, kAXPressAction as CFString)
                Thread.sleep(forTimeInterval: 0.5)
                // In the grouped form a switch has no title: its label is the text element just before it.
                let flat = all(win, { role($0) == "AXCheckBox" || role($0) == "AXStaticText" })
                let at = flat.firstIndex { role($0) == "AXStaticText" && ((attr($0, kAXValueAttribute) as? String) ?? label($0)).contains("Keep pointer speed") }
                let sw = all(win, { role($0) == "AXCheckBox" && label($0).contains("Keep pointer speed") }).first
                    ?? at.flatMap { i in flat[(i + 1)...].first { role($0) == "AXCheckBox" } }
                if let sw {
                    let before = configBool("keepPointerSpeed") ?? true   // absent = the default
                    AXUIElementPerformAction(sw, kAXPressAction as CFString); Thread.sleep(forTimeInterval: 0.6)
                    let after = configBool("keepPointerSpeed")
                    AXUIElementPerformAction(sw, kAXPressAction as CFString); Thread.sleep(forTimeInterval: 0.6)
                    let restored = configBool("keepPointerSpeed")
                    note((after == !before && restored == before ? "PASS" : "FAIL") + " switch writes config: \(String(describing: before)) -> \(String(describing: after)) -> \(String(describing: restored))")
                } else { note("FAIL keep-speed switch not found") }
            } else { note("FAIL Mouse tab not found") }

            // Zoom tab: nudge the first slider (zoom-in speed) up and back down.
            if let zoomTab = tabs.first(where: { label($0).contains("Zoom") }) {
                AXUIElementPerformAction(zoomTab, kAXPressAction as CFString)
                Thread.sleep(forTimeInterval: 0.5)
                if let slider = all(win, { role($0) == "AXSlider" }).first {
                    let before = configDouble("zoomInSpeed") ?? 1.75     // absent = the default
                    AXUIElementPerformAction(slider, kAXIncrementAction as CFString); Thread.sleep(forTimeInterval: 0.6)
                    let after = configDouble("zoomInSpeed")
                    AXUIElementPerformAction(slider, kAXDecrementAction as CFString); Thread.sleep(forTimeInterval: 0.6)
                    let restored = configDouble("zoomInSpeed")
                    let ok = after != nil && after! > before && abs((restored ?? 0) - before) < 0.001
                    note((ok ? "PASS" : "FAIL") + " slider writes config: \(String(describing: before)) -> \(String(describing: after)) -> \(String(describing: restored))")
                } else { note("FAIL no slider on the Zoom tab") }
            }
            var close: CFTypeRef?
            if AXUIElementCopyAttributeValue(win, kAXCloseButtonAttribute as CFString, &close) == .success, let close {
                AXUIElementPerformAction(close as! AXUIElement, kAXPressAction as CFString)
            }
            return log
    }

    private static func attr(_ el: AXUIElement, _ a: String) -> AnyObject? {
        var v: CFTypeRef?
        return AXUIElementCopyAttributeValue(el, a as CFString, &v) == .success ? v : nil
    }
    private static func role(_ el: AXUIElement) -> String { attr(el, kAXRoleAttribute) as? String ?? "" }
    private static func label(_ el: AXUIElement) -> String {
        [kAXTitleAttribute, kAXDescriptionAttribute, kAXHelpAttribute].compactMap { attr(el, $0) as? String }.joined(separator: " ")
    }
    private static func all(_ el: AXUIElement, _ pred: (AXUIElement) -> Bool, depth: Int = 0) -> [AXUIElement] {
        if depth > 30 { return [] }
        var out: [AXUIElement] = pred(el) ? [el] : []
        for k in (attr(el, kAXChildrenAttribute) as? [AXUIElement]) ?? [] { out += all(k, pred, depth: depth + 1) }
        return out
    }
    private static func config() -> [String: Any] {
        (try? JSONSerialization.jsonObject(with: Data(contentsOf: Paths.config))) as? [String: Any] ?? [:]
    }
    private static func configBool(_ k: String) -> Bool? { config()[k] as? Bool }
    private static func configDouble(_ k: String) -> Double? { config()[k] as? Double }
}

#endif
