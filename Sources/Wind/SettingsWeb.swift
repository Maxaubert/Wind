import AppKit
import WebKit
import WindCore

// The Settings window: Windows Wind's Svelte settings UI (ui/, built into Resources/ui) in a WKWebView.
// The page talks to this host through window.webkit.messageHandlers.wind (page -> host) and
// window.__windReceive(json) (host -> page); the message set follows WindConfig.exe's
// (Windows src/config_ui/main.cpp), minus profiles and sessions (the Mac applies every change at once),
// plus Mac messages (getMacStatus, macAction, captureBinds). Every change is written to config.json
// through the app's save path and applies live.

final class SettingsWindowController: NSObject, WKScriptMessageHandler, WKNavigationDelegate, NSWindowDelegate {
    private var window: NSWindow?
    private var web: WKWebView?
    private var config = Config()
    private var save: ((Config) -> Void)?
    private var fixZoom: (() -> Void)?
    /// Pause Wind's own hotkeys while the page records a keybind.
    var onCapture: ((Bool) -> Void)?
    /// The engine Wind runs now ("draw" or "zoom"), for the page's engine row.
    var runningEngine: () -> String = { "" }
    /// Mac status for the macOS card and the banner.
    var macStatus: () -> [String: Any] = { [:] }
    /// macOS card actions (open System Settings panes, login item, show log, run setup again...).
    var macAction: ((String, String) -> Void)?

    /// Show the settings (or the first-run setup with `onboard`).
    func show(config: Config, save: @escaping (Config) -> Void, fixZoom: @escaping () -> Void, onboard: Bool = false) {
        self.config = config; self.save = save; self.fixZoom = fixZoom
        if onboard { onboarding = true }
        if let w = window, let web {
            if onboard { load(web, onboard: true) }
            present(w)
            return
        }
        let cfg = WKWebViewConfiguration()
        cfg.setURLSchemeHandler(UIFiles(), forURLScheme: "wind")
        let ucc = WKUserContentController()
        ucc.add(WeakHandler(self), name: "wind")
        cfg.userContentController = ucc
        let web = WKWebView(frame: NSRect(x: 0, y: 0, width: 1040, height: 720), configuration: cfg)
        web.navigationDelegate = self
        web.setValue(false, forKey: "drawsBackground")           // the window's dark fill shows until the page paints
        let w = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 1040, height: 720),
                         styleMask: [.titled, .closable, .miniaturizable, .resizable, .fullSizeContentView],
                         backing: .buffered, defer: false)
        w.titlebarAppearsTransparent = true
        w.titleVisibility = .hidden
        w.title = "Wind Settings"
        w.backgroundColor = NSColor(srgbRed: 0.09, green: 0.09, blue: 0.10, alpha: 1)
        w.appearance = NSAppearance(named: .darkAqua)
        w.minSize = NSSize(width: 700, height: 480)
        w.isReleasedWhenClosed = false
        w.contentView = web
        w.delegate = self
        w.center()
        window = w; self.web = web
        installEditMenu()
        load(web, onboard: onboard)
        present(w)
    }

    private func present(_ w: NSWindow) {
        #if WIND_TEST_CHANNEL
        // Tests (settings offscreen on): show the window off every display, without taking focus, so
        // a test run never covers the owner's work. screencapture -l still captures it.
        if SettingsWindowController.testOffscreen {
            w.orderFrontRegardless()
            w.setFrameOrigin(NSPoint(x: -30000, y: -30000))
            return
        }
        #endif
        NSApp.activate(ignoringOtherApps: true)
        w.makeKeyAndOrderFront(nil)
    }

    /// Keep an open window in step with config changes made elsewhere (menu, text editor).
    func update(_ c: Config) {
        guard c != config else { return }
        config = c
        send(["type": "config", "values": values(c), "saved": values(c), "runningModel": runningEngine(), "push": true])
    }

    /// Push the macOS status (a permission granted, the login item changed...).
    func pushMacStatus() {
        let st = macStatus()
        lastStatus = json(st)
        send(["type": "macStatus"].merging(st) { a, _ in a })
    }

    /// Push the macOS status only when it differs from the last one sent (polled while the window is open).
    func pushMacStatusIfChanged() {
        if json(macStatus()) != lastStatus { pushMacStatus() }
    }
    private var lastStatus = ""
    /// The window shows the first-run setup: closing it counts as skipping (writes onboarded).
    private var onboarding = false

    var isOpen: Bool { window?.isVisible == true }

    private func load(_ web: WKWebView, onboard: Bool) {
        // First paint without a round trip: the page reads window.__windInit once (bridge.js getSession).
        let initJSON = json(["values": values(config), "saved": values(config), "runningModel": runningEngine(),
                             "mac": macStatus()])
        let ucc = web.configuration.userContentController
        ucc.removeAllUserScripts()
        ucc.addUserScript(WKUserScript(source: "window.__windInit = \(initJSON);", injectionTime: .atDocumentStart, forMainFrameOnly: true))
        web.load(URLRequest(url: URL(string: "wind://ui/index.html" + (onboard ? "?mode=onboard" : ""))!))
    }

    // MARK: messages

    func userContentController(_ ucc: WKUserContentController, didReceive message: WKScriptMessage) {
        guard let m = message.body as? [String: Any], let type = m["type"] as? String else { return }
        let s = { (k: String) in (m[k] as? String) ?? (m[k].map { "\($0)" } ?? "") }
        switch type {
        case "ready": break
        case "getConfig":
            send(["type": "config", "values": values(config), "saved": values(config), "runningModel": runningEngine()])
        case "setConfig", "setConfigPersist":
            let key = s("key")
            if let c = setting(config, key, s("value")) {
                config = c
                save?(c)
            } else {
                Log.error("settings: refused \(key)=\(s("value"))")
                send(["type": "configWriteFailed", "key": key])
            }
        case "window":
            switch s("action") {
            case "close": window?.performClose(nil)
            case "minimize": window?.miniaturize(nil)
            case "quitWind": NSApp.terminate(nil)
            default: break
            }
        case "openIni": NSWorkspace.shared.open(Paths.config)
        case "exportDiagnostics": exportDiagnostics()
        case "getMacStatus": pushMacStatus()
        case "macAction":
            if s("action") == "fixZoomShortcuts" { fixZoom?() } else { macAction?(s("action"), s("value")) }
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.3) { [weak self] in self?.pushMacStatus() }
        case "captureBinds": onCapture?(s("on") == "1")
        case "dirty", "saveSession", "discardSession": break   // no sessions on the Mac
        default: Log.verbose("settings: unknown message \(type)")
        }
    }

    private func send(_ msg: [String: Any]) {
        guard let web else { return }
        web.evaluateJavaScript("window.__windReceive && window.__windReceive(\(json(msg)))")
    }

    /// The config as the page sees it: every value a string (as Windows' ini), bools "1"/"0", bind
    /// lists comma-joined.
    func values(_ c: Config) -> [String: String] {
        guard let d = try? JSONSerialization.jsonObject(with: c.encoded()) as? [String: Any] else { return [:] }
        var out: [String: String] = [:]
        for (k, v) in d {
            if let n = v as? NSNumber {
                if CFGetTypeID(n) == CFBooleanGetTypeID() { out[k] = n.boolValue ? "1" : "0" }
                else { let x = n.doubleValue; out[k] = x == x.rounded() && abs(x) < 1e15 ? String(Int(x)) : String(x) }
            } else if let a = v as? [String] { out[k] = a.joined(separator: ",") }
            else if let str = v as? String { out[k] = str }
        }
        return out
    }

    /// A config with `key` set from the page's string, by the key's type in Config; nil if the key is
    /// unknown or the value does not parse.
    func setting(_ c: Config, _ key: String, _ value: String) -> Config? {
        guard var d = try? JSONSerialization.jsonObject(with: c.encoded()) as? [String: Any], let old = d[key] else { return nil }
        if let n = old as? NSNumber {
            if CFGetTypeID(n) == CFBooleanGetTypeID() {
                guard ["1", "0", "true", "false"].contains(value) else { return nil }
                d[key] = value == "1" || value == "true"
            } else {
                guard let x = Double(value), x.isFinite else { return nil }
                d[key] = x
            }
        } else if old is [Any] {
            d[key] = value.split(separator: ",").map { $0.trimmingCharacters(in: .whitespaces) }.filter { !$0.isEmpty }
        } else if old is String {
            d[key] = value
        } else { return nil }
        guard let data = try? JSONSerialization.data(withJSONObject: d) else { return nil }
        var dropped: [String] = []
        guard var out = try? Config.load(from: data, dropped: &dropped), dropped.isEmpty else { return nil }
        out.clamp()
        return out
    }

    private func exportDiagnostics() {
        let f = DateFormatter(); f.dateFormat = "yyyyMMdd-HHmmss"
        let desk = FileManager.default.urls(for: .desktopDirectory, in: .userDomainMask)[0]
        let zip = desk.appendingPathComponent("Wind-diagnostics-\(f.string(from: Date())).zip")
        let tmp = FileManager.default.temporaryDirectory.appendingPathComponent("wind-diag-\(UUID().uuidString)")
        DispatchQueue.global(qos: .utility).async {
            let fm = FileManager.default
            try? fm.createDirectory(at: tmp, withIntermediateDirectories: true)
            try? fm.copyItem(at: Paths.logDir, to: tmp.appendingPathComponent("logs"))
            try? fm.copyItem(at: Paths.config, to: tmp.appendingPathComponent("config.json"))
            let p = Process()
            p.executableURL = URL(fileURLWithPath: "/usr/bin/ditto")
            p.arguments = ["-c", "-k", "--keepParent", tmp.path, zip.path]
            try? p.run(); p.waitUntilExit()
            try? fm.removeItem(at: tmp)
            DispatchQueue.main.async {
                if fm.fileExists(atPath: zip.path) { NSWorkspace.shared.activateFileViewerSelecting([zip]) }
                Log.info("diagnostics exported to \(zip.lastPathComponent)")
            }
        }
    }

    // MARK: window and navigation

    func webView(_ webView: WKWebView, decidePolicyFor action: WKNavigationAction, decisionHandler: @escaping (WKNavigationActionPolicy) -> Void) {
        // The page only; anything else (a link, a dropped file) is refused.
        decisionHandler(action.request.url?.scheme == "wind" ? .allow : .cancel)
    }

    func webViewWebContentProcessDidTerminate(_ webView: WKWebView) {
        Log.error("settings: the web content process ended; reloading")
        load(webView, onboard: false)
    }

    func windowWillClose(_ n: Notification) {
        onCapture?(false)
        if onboarding && !config.onboarded {
            config.onboarded = true
            save?(config)
        }
        onboarding = false
        window?.contentView = nil
        web = nil; window = nil
    }

    /// An agent app has no menu bar, so the web view would get no Cut/Copy/Paste/Select All/Close keys.
    private func installEditMenu() {
        guard NSApp.mainMenu == nil else { return }
        let main = NSMenu()
        let appItem = NSMenuItem(); main.addItem(appItem)
        let app = NSMenu(); appItem.submenu = app
        app.addItem(withTitle: "Close Window", action: #selector(NSWindow.performClose(_:)), keyEquivalent: "w")
        let editItem = NSMenuItem(); main.addItem(editItem)
        let edit = NSMenu(title: "Edit"); editItem.submenu = edit
        edit.addItem(withTitle: "Undo", action: Selector(("undo:")), keyEquivalent: "z")
        edit.addItem(withTitle: "Redo", action: Selector(("redo:")), keyEquivalent: "Z")
        edit.addItem(withTitle: "Cut", action: #selector(NSText.cut(_:)), keyEquivalent: "x")
        edit.addItem(withTitle: "Copy", action: #selector(NSText.copy(_:)), keyEquivalent: "c")
        edit.addItem(withTitle: "Paste", action: #selector(NSText.paste(_:)), keyEquivalent: "v")
        edit.addItem(withTitle: "Select All", action: #selector(NSText.selectAll(_:)), keyEquivalent: "a")
        NSApp.mainMenu = main
    }

    private func json(_ o: Any) -> String {
        guard let d = try? JSONSerialization.data(withJSONObject: o, options: [.sortedKeys]), let s = String(data: d, encoding: .utf8) else { return "{}" }
        // U+2028/2029 are valid JSON but end a line in older JavaScript; escape them for evaluateJavaScript.
        return s.replacingOccurrences(of: "\u{2028}", with: "\\u2028").replacingOccurrences(of: "\u{2029}", with: "\\u2029")
    }
}

/// WKUserContentController retains its handlers; this breaks the cycle with the controller.
private final class WeakHandler: NSObject, WKScriptMessageHandler {
    weak var target: WKScriptMessageHandler?
    init(_ t: WKScriptMessageHandler) { target = t }
    func userContentController(_ ucc: WKUserContentController, didReceive message: WKScriptMessage) {
        target?.userContentController(ucc, didReceive: message)
    }
}

/// Serves the built UI (Resources/ui) at wind://ui/... : ES modules do not load from file URLs.
private final class UIFiles: NSObject, WKURLSchemeHandler {
    static var root: URL? {
        if let r = Bundle.main.resourceURL?.appendingPathComponent("ui"), FileManager.default.fileExists(atPath: r.path) { return r }
        return nil
    }

    func webView(_ webView: WKWebView, start task: WKURLSchemeTask) {
        guard let url = task.request.url, let root = UIFiles.root else { return fail(task) }
        let rel = url.path.hasPrefix("/") ? String(url.path.dropFirst()) : url.path
        let file = root.appendingPathComponent(rel.isEmpty ? "index.html" : rel).standardizedFileURL
        guard file.path.hasPrefix(root.standardizedFileURL.path), let data = try? Data(contentsOf: file) else { return fail(task) }
        let types = ["html": "text/html", "js": "text/javascript", "mjs": "text/javascript", "css": "text/css",
                     "svg": "image/svg+xml", "png": "image/png", "jpg": "image/jpeg", "json": "application/json",
                     "woff2": "font/woff2"]
        let mime = types[file.pathExtension.lowercased()] ?? "application/octet-stream"
        task.didReceive(HTTPURLResponse(url: url, statusCode: 200, httpVersion: "HTTP/1.1",
                                        headerFields: ["Content-Type": mime, "Content-Length": "\(data.count)"])!)
        task.didReceive(data)
        task.didFinish()
    }

    func webView(_ webView: WKWebView, stop task: WKURLSchemeTask) {}

    private func fail(_ task: WKURLSchemeTask) {
        task.didFailWithError(NSError(domain: NSURLErrorDomain, code: NSURLErrorFileDoesNotExist))
    }
}

#if WIND_TEST_CHANNEL
/// Test builds only (scripts/e2e_settings.py through the command channel): run JavaScript in the
/// settings page and write the result to js.json, and report the window for screenshots.
extension SettingsWindowController {
    nonisolated(unsafe) static var testOffscreen = false
    static let testReply = Paths.support.appendingPathComponent("js.json")

    /// The window's CGWindowID (screencapture -l), 0 when closed.
    var testWindowId: Int { window?.windowNumber ?? 0 }
    var testLoadedURL: String { web?.url?.absoluteString ?? "" }

    /// Runs `body` as the body of an async function in the page (it may await and should return a
    /// JSON-serialisable value), then writes {ok, value (JSON text), error, windowId, url} to js.json.
    func testEval(_ body: String) {
        func reply(_ d: [String: Any]) {
            var d = d
            d["windowId"] = testWindowId; d["url"] = testLoadedURL
            if let data = try? JSONSerialization.data(withJSONObject: d, options: [.sortedKeys]) {
                try? data.write(to: SettingsWindowController.testReply, options: .atomic)
            }
        }
        guard let web else { return reply(["ok": false, "error": "settings window not open"]) }
        let js = "const __r = await (async () => { \(body) })(); return JSON.stringify(__r === undefined ? null : __r);"
        web.callAsyncJavaScript(js, arguments: [:], in: nil, in: .page) { result in
            switch result {
            case .success(let v): reply(["ok": true, "value": (v as? String) ?? "null"])
            case .failure(let e): reply(["ok": false, "error": "\(e)"])
            }
        }
    }

    func testClose() { window?.performClose(nil) }
}
#endif
