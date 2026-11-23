// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
import AppKit
import WebKit
import WindCore

// The menu bar panel: a left click on the status item opens it (a right or Ctrl click shows the small
// native menu, AppDelegate). The Mac counterpart of Windows' tray flyout (src/tray_app/flyout_*): the
// Performance header, up to four sliders, the toggle group and engine picker, Settings and Quit, in the
// order Settings > Menu bar sets (menu* keys, MenuLayout). The page is ui/panel.html (Svelte, the same
// wind://ui/ files and bridge as Settings) in a WKWebView inside a borderless panel. Built on the first
// open and then kept, hidden, so later opens show at once. Placement and the re-open guard are pure
// (WindCore/MenuPanel.swift). Lessons from Universal Clipboard's Mac panel (7ed027a, 3bb6252): activate
// the app on open or WebKit gets no mouse-moved events (no hover, no tooltips); no native shadow, the
// page draws it; close on focus loss, Esc, a second icon click, Settings and Quit. A close Wind itself
// caused (Esc, the icon, the page's close) hands activation back to the app that had it, so the text
// field Wind follows keeps its keystrokes.
//
// Page -> host (webkit.messageHandlers.wind): panelSize {h}; getConfig; setConfig {key, value};
// setConfigs {values}; getMacStatus; panelAction {action: close | openSettings | quit | openPrivacy |
// fixZoomShortcuts | setEngine, value}; hover {id}.
// Host -> page (window.__windReceive): config {values, push}; macStatus {...}; perf {level, ms[]};
// panel {open}. First paint: window.__windInit {values, mac, perf}.

/// Borderless, so it must say it can take key focus itself (the sliders want the arrow keys).
final class MenuPanelWindow: NSPanel {
    var onEscape: (() -> Void)?
    override var canBecomeKey: Bool { true }
    override var canBecomeMain: Bool { false }
    override func cancelOperation(_ sender: Any?) { onEscape?() }   // Esc the page did not take
}

final class MenuBarPanelController: NSObject, WKScriptMessageHandler, WKNavigationDelegate, NSWindowDelegate {
    /// The config Wind runs on now (AppDelegate's mag.config).
    var currentConfig: () -> Config = { Config() }
    /// Writes a config through AppDelegate.saveConfig (clamp, write config.json, reload, apply).
    var save: ((Config) -> Void)?
    var macStatus: () -> [String: Any] = { [:] }
    /// The zoom level and the tick ring's samples (ms, oldest first) for the Performance header.
    var perf: () -> (level: Double, ms: [Float]) = { (1, []) }
    /// openSettings | quit | openPrivacy | fixZoomShortcuts
    var action: ((String) -> Void)?
    /// The status item button's frame in screen coordinates (nil while it is not shown).
    var anchor: () -> CGRect? = { nil }

    private var panel: MenuPanelWindow?
    private var web: WKWebView?
    private var pageHeight: CGFloat = 0           // reported by the page; 0 until its first report
    private var pendingShow = false
    private(set) var isOpen = false
    private var closedAt: Double = 0
    private var perfTimer: Timer?
    private var lastStatus = ""
    private var pushed: Config?                   // the config the page shows
    /// Writes are coalesced: at most one per 50 ms, flushed by a timer and at close (Windows' WriteThrottle).
    /// Only the keys the page changed wait here, in arrival order, and the flush applies them to the config
    /// Wind runs on at that moment, so a change made elsewhere meanwhile (Settings, config.json, a hotkey)
    /// is kept. The page's sliders throttle the same way and flush on release.
    private var pending: [(key: String, value: String)] = []
    private var lastWrite: Double = 0
    private var flushTimer: Timer?
    static let writeGap = 0.05
    /// The control under the pointer (the page reports it), for the test state.
    private(set) var hovered = ""
    /// The app that was frontmost when the panel opened (nil when it was Wind), and whether Wind was active.
    private var previousApp: NSRunningApplication?
    private var wasActive = false

    // MARK: open and close

    /// A left click on the icon.
    func toggle() {
        if isOpen { close("icon"); return }
        if MenuPanel.ignoreClick(now: CACurrentMediaTime(), closedAt: closedAt) {
            Log.verbose("menu bar panel: the click that closed it, not reopening")
            return
        }
        open()
    }

    func open() {
        guard !isOpen else { return }
        let p = ensure()
        isOpen = true
        update(currentConfig())
        pushMacStatus()
        sendPerf()
        send(["type": "panel", "open": true])
        startPerf()
        // An accessory app is not activated by a status item click; without this WebKit gets no
        // mouse-moved events, so nothing on the page shows hover (Universal Clipboard 7ed027a). The
        // activation policy stays .accessory: no Dock icon. close() gives activation back.
        wasActive = NSApp.isActive
        let front = NSWorkspace.shared.frontmostApplication
        previousApp = front?.processIdentifier == ProcessInfo.processInfo.processIdentifier ? nil : front
        NSApp.activate()
        if pageHeight > 0 {
            show(p)
        } else {
            // The first open waits for the page to report its height, so it never shows at a wrong size.
            pendingShow = true
            DispatchQueue.main.asyncAfter(deadline: .now() + 1.5) { [weak self] in
                guard let self, self.pendingShow, let p = self.panel else { return }
                Log.error("menu bar panel: the page did not report its size; showing it anyway")
                self.show(p)
            }
        }
    }

    private func show(_ p: MenuPanelWindow) {
        pendingShow = false
        guard isOpen else { return }
        place()
        p.makeKeyAndOrderFront(nil)
        if let web { p.makeFirstResponder(web) }
    }

    func close(_ why: String) {
        guard isOpen else { return }
        isOpen = false
        pendingShow = false
        closedAt = CACurrentMediaTime()
        perfTimer?.invalidate(); perfTimer = nil
        send(["type": "panel", "open": false])    // the page drops focus, so a slider's last value arrives
        panel?.orderOut(nil)
        giveBackFocus(why)
        flush()
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.15) { [weak self] in self?.flush() }
        Log.verbose("menu bar panel: closed (\(why))")
    }

    /// After a close Wind caused, the app that was frontmost before the panel opened gets activation back.
    /// Not on focus loss (another app already took it), Settings (it comes forward) or Quit, and not when
    /// Wind was active already (its Settings window was in front).
    private func giveBackFocus(_ why: String) {
        let app = previousApp
        previousApp = nil
        guard !["focus", "openSettings", "quit"].contains(why), !wasActive, NSApp.isActive else { return }
        if let app, !app.isTerminated {
            NSApp.yieldActivation(to: app)
            app.activate()
        } else {
            NSApp.deactivate()
        }
    }

    /// Size and place the window from the page's height, under the status item.
    private func place() {
        guard let p = panel else { return }
        let icon = anchor() ?? CGRect(x: (NSScreen.main?.frame.maxX ?? 800) - 40, y: NSScreen.main?.frame.maxY ?? 800, width: 30, height: 24)
        let screen = NSScreen.screens.first { $0.frame.contains(CGPoint(x: icon.midX, y: icon.midY)) }
            ?? NSScreen.screens.first { $0.frame.intersects(icon) } ?? NSScreen.main
        guard let vis = screen?.visibleFrame else { return }
        let f = MenuPanel.frame(icon: icon, visible: vis, height: pageHeight > 0 ? pageHeight : 420)
        if p.frame != f { p.setFrame(f, display: true) }
    }

    // MARK: building

    private func ensure() -> MenuPanelWindow {
        if let panel { return panel }
        let cfg = WKWebViewConfiguration()
        cfg.setURLSchemeHandler(UIFiles(), forURLScheme: "wind")
        let ucc = WKUserContentController()
        ucc.add(WeakHandler(self), name: "wind")
        cfg.userContentController = ucc
        let rect = NSRect(x: 0, y: 0, width: MenuPanel.windowWidth, height: 420)
        let web = WKWebView(frame: rect, configuration: cfg)
        web.setValue(false, forKey: "drawsBackground")             // clear: the page draws the card and its shadow
        web.underPageBackgroundColor = .clear
        web.navigationDelegate = self
        web.autoresizingMask = [.width, .height]
        let p = MenuPanelWindow(contentRect: rect, styleMask: [.borderless], backing: .buffered, defer: false)
        p.title = "Wind Menu Bar"
        p.isFloatingPanel = true
        p.level = .floating
        p.isOpaque = false
        p.backgroundColor = .clear
        p.hasShadow = false
        p.hidesOnDeactivate = false
        p.isReleasedWhenClosed = false
        p.becomesKeyOnlyIfNeeded = false
        p.appearance = NSAppearance(named: .darkAqua)
        p.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary, .transient, .ignoresCycle]
        p.contentView = web
        p.delegate = self
        p.onEscape = { [weak self] in self?.close("esc") }
        panel = p; self.web = web
        load(web)
        return p
    }

    private func load(_ web: WKWebView) {
        let c = currentConfig()
        pushed = c
        let pf = perf()
        let initJSON = SettingsWindowController.jsonText(["values": SettingsWindowController.values(c), "mac": macStatus(),
                                                          "perf": ["level": pf.level, "ms": rounded(pf.ms)]])
        let ucc = web.configuration.userContentController
        ucc.removeAllUserScripts()
        ucc.addUserScript(WKUserScript(source: "window.__windInit = \(initJSON);", injectionTime: .atDocumentStart, forMainFrameOnly: true))
        web.load(URLRequest(url: URL(string: "wind://ui/panel.html")!))
    }

    // MARK: host -> page

    /// Keep the page in step with config changes made elsewhere (Settings, a text editor, the hotkeys).
    func update(_ c: Config) {
        guard web != nil, pending.isEmpty, c != pushed else { return }
        pushed = c
        send(["type": "config", "values": SettingsWindowController.values(c), "push": true])
    }

    func pushMacStatus() {
        let st = macStatus()
        lastStatus = SettingsWindowController.jsonText(st)
        send(["type": "macStatus"].merging(st) { a, _ in a })
    }

    func pushMacStatusIfChanged() {
        if isOpen && SettingsWindowController.jsonText(macStatus()) != lastStatus { pushMacStatus() }
    }

    /// About 10 Hz, only while open and only with the Performance header on (Windows re-reads every 100 ms).
    private func startPerf() {
        perfTimer?.invalidate()
        let t = Timer(timeInterval: 0.1, repeats: true) { [weak self] _ in self?.sendPerf() }
        RunLoop.main.add(t, forMode: .common)
        perfTimer = t
    }

    private func sendPerf() {
        guard isOpen, currentConfig().menuPerf else { return }
        let pf = perf()
        send(["type": "perf", "level": pf.level, "ms": rounded(pf.ms)])
    }

    private func rounded(_ ms: [Float]) -> [Double] { ms.map { (Double($0) * 100).rounded() / 100 } }

    private func send(_ msg: [String: Any]) {
        guard let web else { return }
        web.evaluateJavaScript("window.__windReceive && window.__windReceive(\(SettingsWindowController.jsonText(msg)))")
    }

    // MARK: page -> host

    func userContentController(_ ucc: WKUserContentController, didReceive message: WKScriptMessage) {
        guard let m = message.body as? [String: Any], let type = m["type"] as? String else { return }
        let s = { (k: String) in (m[k] as? String) ?? (m[k].map { "\($0)" } ?? "") }
        switch type {
        case "panelSize":
            guard let h = Double(s("h")), h.isFinite, h > 0 else { return }
            pageHeight = CGFloat(h.rounded(.up))
            if pendingShow, let p = panel { show(p) } else if isOpen { place() }
        case "getConfig":
            let c = applied(pending, to: currentConfig()).config
            pushed = c
            send(["type": "config", "values": SettingsWindowController.values(c)])
        case "setConfig":
            write([s("key"): s("value")])
        case "setConfigs":
            guard let v = m["values"] as? [String: Any] else { return }
            write(v.mapValues { ($0 as? String) ?? "\($0)" })
        case "getMacStatus": pushMacStatus()
        case "panelAction":
            let a = s("action")
            switch a {
            case "close": close("esc")
            case "setEngine": write(["engine": s("value")], now: true)
            case "openSettings", "quit":
                close(a)
                action?(a)
            case "openPrivacy", "fixZoomShortcuts":
                action?(a)
                DispatchQueue.main.asyncAfter(deadline: .now() + 0.3) { [weak self] in self?.pushMacStatus() }
            default: Log.verbose("menu bar panel: unknown action \(a)")
            }
        case "hover": hovered = s("id")
        default: Log.verbose("menu bar panel: unknown message \(type)")
        }
    }

    /// `kv` applied in order to `c` through the Settings typing; the keys it refused are left out.
    private func applied(_ kv: [(key: String, value: String)], to c: Config) -> (config: Config, refused: [String]) {
        var c = c, refused: [String] = []
        for (k, v) in kv {
            if let next = SettingsWindowController.setting(c, k, v) { c = next } else { refused.append(k) }
        }
        return (c, refused)
    }

    /// Checks page writes against the latest config (pending keys first) and queues the keys.
    private func write(_ kv: [String: String], now: Bool = false) {
        let keys = kv.sorted(by: { $0.key < $1.key }).map { (key: $0.key, value: $0.value) }
        let r = applied(pending + keys, to: currentConfig())
        if let k = r.refused.first(where: { k in keys.contains { $0.key == k } }) {
            Log.error("menu bar panel: refused \(k)=\(kv[k] ?? "")")
            send(["type": "configWriteFailed", "key": k])
            return
        }
        for (k, v) in keys {
            pending.removeAll { $0.key == k }
            pending.append((k, v))
        }
        pushed = r.config                              // what the page shows now
        let t = CACurrentMediaTime()
        if now || t - lastWrite >= MenuBarPanelController.writeGap { flush(); return }
        if flushTimer == nil {
            let f = Timer(timeInterval: MenuBarPanelController.writeGap - (t - lastWrite), repeats: false) { [weak self] _ in
                self?.flushTimer = nil
                self?.flush()
            }
            RunLoop.main.add(f, forMode: .common)
            flushTimer = f
        }
    }

    func flush() {
        flushTimer?.invalidate(); flushTimer = nil
        guard !pending.isEmpty else { return }
        let kv = pending
        pending = []
        lastWrite = CACurrentMediaTime()
        // Applied to the config Wind runs on now, so only these keys change. Saving reloads and calls
        // update(), which pushes the result to the page if it differs from what the page showed.
        let r = applied(kv, to: currentConfig())
        for k in r.refused {
            Log.error("menu bar panel: refused \(k) at flush")
            send(["type": "configWriteFailed", "key": k])
        }
        save?(r.config)
    }

    // MARK: window and navigation

    func windowDidResignKey(_ n: Notification) { close("focus") }

    func webView(_ webView: WKWebView, decidePolicyFor action: WKNavigationAction, decisionHandler: @escaping (WKNavigationActionPolicy) -> Void) {
        decisionHandler(action.request.url?.scheme == "wind" ? .allow : .cancel)
    }

    func webViewWebContentProcessDidTerminate(_ webView: WKWebView) {
        Log.error("menu bar panel: the web content process ended; reloading")
        pageHeight = 0
        load(webView)
    }
}

#if WIND_TEST_CHANNEL
/// Test builds only (scripts/e2e_menubar.py): the panel's state for state.json, and JavaScript in its page.
extension MenuBarPanelController {
    static let testReply = Paths.support.appendingPathComponent("panel-js.json")

    /// CG global coordinates (origin top-left of the main display, as CGWindowList and `warp`).
    private func cg(_ r: CGRect) -> [Double] {
        let h = NSScreen.screens.first?.frame.height ?? 0
        return [r.minX, h - r.maxY, r.width, r.height].map { Double($0) }
    }

    func testState() -> [String: Any] {
        let vis = panel?.screen?.visibleFrame ?? NSScreen.main?.visibleFrame ?? .zero
        return ["open": isOpen && panel?.isVisible == true, "key": panel?.isKeyWindow == true,
                "frame": panel.map { cg($0.frame) } ?? [], "card": panel.map { cg(MenuPanel.card(in: $0.frame)) } ?? [],
                "statusItem": anchor().map(cg) ?? [], "visible": cg(vis), "hovered": hovered,
                "windowId": panel?.windowNumber ?? 0, "pageHeight": Double(pageHeight), "active": NSApp.isActive]
    }

    /// Runs `body` as an async function body in the panel page; {ok, value (JSON text), error} to panel-js.json.
    func testEval(_ body: String) {
        func reply(_ d: [String: Any]) {
            if let data = try? JSONSerialization.data(withJSONObject: d, options: [.sortedKeys]) {
                try? data.write(to: MenuBarPanelController.testReply, options: .atomic)
            }
        }
        guard let web else { return reply(["ok": false, "error": "panel not built"]) }
        let js = "const __r = await (async () => { \(body) })(); return JSON.stringify(__r === undefined ? null : __r);"
        web.callAsyncJavaScript(js, arguments: [:], in: nil, in: .page) { result in
            switch result {
            case .success(let v): reply(["ok": true, "value": (v as? String) ?? "null"])
            case .failure(let e): reply(["ok": false, "error": "\(e)"])
            }
        }
    }
}
#endif
