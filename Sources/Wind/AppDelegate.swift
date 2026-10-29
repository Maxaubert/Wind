import AppKit
import ApplicationServices
import WindCore

final class AppDelegate: NSObject, NSApplicationDelegate, NSMenuDelegate {
    let mag = Magnifier()
    private var statusItem: NSStatusItem!
    private var pollTimer: Timer?
    private var configMTime: Date?
    private var started = false
    private var signalSources: [DispatchSourceSignal] = []
    /// The command channel lets any process running as this user drive Wind's Accessibility-backed
    /// input, so it only exists when Wind is launched for testing (`--test-channel`).
    private let commandChannel = CommandLine.arguments.contains("--test-channel")
    private var zoomShortcuts: [String] = []     // macOS Zoom shortcuts that are on (issue #1)
    private var polls = 0

    func applicationDidFinishLaunching(_ n: Notification) {
        Log.info("Wind \(Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "dev") starting, pid \(getpid())")
        installSignalHandlers()
        Watchdog.spawn()
        statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
        statusItem.button?.image = NSImage(systemSymbolName: "plus.magnifyingglass", accessibilityDescription: "Wind")
        statusItem.button?.imagePosition = .imageLeading
        let menu = NSMenu()
        menu.delegate = self
        statusItem.menu = menu

        guard SkyLight.available else {
            alert("Wind cannot zoom on this macOS version", "The system zoom functions Wind uses were not found.")
            NSApp.terminate(nil)
            return
        }
        loadConfig(force: true)
        mag.onLevelChange = { [weak self] lv in self?.showLevel(lv) }
        mag.onQuitRequested = { NSApp.terminate(nil) }
        pollTimer = Timer.scheduledTimer(withTimeInterval: 0.25, repeats: true) { [weak self] _ in self?.poll() }
        tryStart(prompt: true)
    }

    func applicationWillTerminate(_ n: Notification) {
        mag.stop()
        Log.info("Wind quit")
    }

    private func installSignalHandlers() {
        for s in [SIGTERM, SIGINT, SIGHUP] {
            signal(s, SIG_IGN)
            let src = DispatchSource.makeSignalSource(signal: s, queue: .main)
            src.setEventHandler { NSApp.terminate(nil) }
            src.resume()
            signalSources.append(src)
        }
    }

    // MARK: permission and start

    private func tryStart(prompt: Bool) {
        let opts = [kAXTrustedCheckOptionPrompt.takeUnretainedValue() as String: prompt] as CFDictionary
        guard AXIsProcessTrustedWithOptions(opts) else {
            if prompt { Log.info("waiting for Accessibility permission") }
            statusItem.button?.title = " needs access"
            return
        }
        guard mag.start() else {
            statusItem.button?.title = " needs access"
            return
        }
        started = true
        statusItem.button?.title = ""
        checkSystemZoom()
        Log.info("started; binds: " + Action.allCases.map { a in
            "\(a)=\((mag.config.binds().binds[a] ?? []).map { $0.description }.joined(separator: "|"))" }.joined(separator: " "))
    }

    private func poll() {
        if !started { tryStart(prompt: false) }
        if polls % 8 == 0 { checkSystemZoom() }
        polls += 1
        loadConfig(force: false)
        if commandChannel { runCommandFile() }
    }

    // MARK: macOS Zoom clash (issue #1)

    private func checkSystemZoom() {
        let on = SystemZoom.shortcutsOn()
        if on != zoomShortcuts {
            zoomShortcuts = on
            if on.isEmpty { Log.info("macOS Zoom shortcuts: all off") }
            else { Log.info("macOS Zoom shortcuts on (they clash with Wind): \(on.joined(separator: ", "))") }
            showLevel(mag.level)
        }
    }

    @objc private func turnOffSystemZoom() {
        SystemZoom.turnOffShortcuts { [weak self] ok in
            Log.info("turn off macOS Zoom shortcuts: \(ok ? "done" : "incomplete")")
            self?.checkSystemZoom()
            if !ok {
                self?.alert("Could not switch off every macOS Zoom shortcut",
                            "Open System Settings > Accessibility > Zoom and switch off the keyboard shortcuts, trackpad gesture and scroll gesture.")
            }
        }
    }

    @objc private func toggleShake() {
        SystemZoom.setShakeToLocate(!SystemZoom.shakeToLocateOn) { ok in
            if !ok { Log.error("shake to locate: switch not found") }
        }
    }

    // MARK: config

    private func loadConfig(force: Bool) {
        let fm = FileManager.default
        if !fm.fileExists(atPath: Paths.config.path) {
            try? Config().encoded().write(to: Paths.config)
        }
        let m = (try? fm.attributesOfItem(atPath: Paths.config.path))?[.modificationDate] as? Date
        if !force && m == configMTime { return }
        configMTime = m
        do {
            let c = try Config.load(from: Data(contentsOf: Paths.config))
            mag.apply(c)
            Log.info("config loaded")
        } catch {
            Log.error("config.json unreadable, keeping the previous settings: \(error.localizedDescription)")
            if force { mag.apply(Config()) }
        }
    }

    private func saveConfig(_ edit: (inout Config) -> Void) {
        var c = mag.config
        edit(&c)
        c.clamp()
        // Write only the keys that differ from the defaults plus what the file already had, so the
        // file stays readable; simplest faithful form is the full config.
        try? c.encoded().write(to: Paths.config)
        loadConfig(force: true)
        configMTime = (try? FileManager.default.attributesOfItem(atPath: Paths.config.path))?[.modificationDate] as? Date
    }

    // MARK: menu

    private func showLevel(_ lv: Double) {
        guard started else { return }
        let warn = zoomShortcuts.isEmpty ? "" : " ⚠︎"
        statusItem.button?.title = (lv > 1.01 ? String(format: " %.1fx", lv) : "") + warn
    }

    func menuNeedsUpdate(_ menu: NSMenu) {
        menu.removeAllItems()
        let c = mag.config
        func item(_ title: String, _ action: Selector?, on: Bool = false, tag: Int = 0) -> NSMenuItem {
            let i = NSMenuItem(title: title, action: action, keyEquivalent: "")
            i.target = self; i.state = on ? .on : .off; i.tag = tag
            return i
        }
        if !started {
            menu.addItem(item("Wind needs the Accessibility permission", nil))
            menu.addItem(item("Open Privacy & Security…", #selector(openPrivacy)))
            menu.addItem(.separator())
        } else {
            let st = item(String(format: "Zoom %.1fx%@", mag.level, mag.isLocked ? " (game lock)" : ""), nil)
            menu.addItem(st)
            menu.addItem(.separator())
        }
        if !zoomShortcuts.isEmpty {
            menu.addItem(item("⚠︎ macOS Zoom shortcuts are on and clash with Wind", nil))
            menu.addItem(item("Turn off macOS Zoom shortcuts", #selector(turnOffSystemZoom)))
            menu.addItem(.separator())
        }
        menu.addItem(item("Enabled", #selector(togglePaused), on: !mag.isPaused))
        menu.addItem(item("Follow text caret", #selector(toggleCaret), on: c.trackCaret))
        menu.addItem(item("Follow keyboard focus", #selector(toggleFocus), on: c.trackFocus))
        menu.addItem(item("Mouse within edges (instead of centred)", #selector(toggleEdge), on: c.mouseAlign == 1))
        menu.addItem(item("Smooth pixels", #selector(toggleSmooth), on: c.smoothImages))
        menu.addItem(item("Follow the mouse in games", #selector(toggleLock), on: c.lockTracking))

        let warm = NSMenu()
        for p in [0, 25, 50, 75, 100] {
            warm.addItem(item(p == 0 ? "Off" : "\(p)%", #selector(setWarmth(_:)), on: Int(c.warmthPct) == p, tag: p))
        }
        let wi = item("Warmth", nil); wi.submenu = warm
        menu.addItem(wi)
        let speed = NSMenu()
        for (i, s) in [1.0, 1.75, 2.5, 3.5].enumerated() {
            speed.addItem(item(["Slow", "Normal", "Fast", "Fastest"][i], #selector(setSpeed(_:)), on: abs(c.zoomInSpeed - s) < 0.01, tag: Int(s * 100)))
        }
        let si = item("Zoom speed", nil); si.submenu = speed
        menu.addItem(si)

        menu.addItem(.separator())
        let binds = NSMenu()
        let (b, problems) = c.binds()
        for a in Action.allCases {
            let txt = (b[a] ?? []).map { $0.description }.joined(separator: ", ")
            binds.addItem(item("\(a.rawValue): \(txt.isEmpty ? "unbound" : txt)", nil))
        }
        if let w = c.wheelMods { binds.addItem(item("wheel zoom: \(formatBind(Bind(mods: w, trigger: .none)))+scroll", nil)) }
        for p in problems { binds.addItem(item("⚠︎ \(p)", nil)) }
        let bi = item("Binds", nil); bi.submenu = binds
        menu.addItem(bi)
        menu.addItem(item("Keep pointer speed when zoomed", #selector(toggleKeepSpeed), on: c.keepPointerSpeed))
        menu.addItem(item("macOS: enlarge pointer when shaken", #selector(toggleShake), on: SystemZoom.shakeToLocateOn))
        menu.addItem(item("Edit settings file…", #selector(openConfig)))
        menu.addItem(item("Show log", #selector(openLog)))
        menu.addItem(.separator())
        let q = item("Quit Wind", #selector(quit))
        menu.addItem(q)
    }

    @objc private func togglePaused() { mag.setPaused(!mag.isPaused) }
    @objc private func toggleCaret() { saveConfig { $0.trackCaret.toggle() } }
    @objc private func toggleFocus() { saveConfig { $0.trackFocus.toggle() } }
    @objc private func toggleEdge() { saveConfig { $0.mouseAlign = $0.mouseAlign == 1 ? 0 : 1 } }
    @objc private func toggleSmooth() { saveConfig { $0.smoothImages.toggle() } }
    @objc private func toggleKeepSpeed() { saveConfig { $0.keepPointerSpeed.toggle() } }
    @objc private func toggleLock() { saveConfig { $0.lockTracking.toggle() } }
    @objc private func setWarmth(_ s: NSMenuItem) { saveConfig { $0.warmthPct = Double(s.tag) } }
    @objc private func setSpeed(_ s: NSMenuItem) {
        saveConfig { $0.zoomInSpeed = Double(s.tag) / 100; $0.zoomOutSpeed = Double(s.tag) / 100 }
    }
    @objc private func openConfig() { NSWorkspace.shared.open(Paths.config) }
    @objc private func openLog() { NSWorkspace.shared.open(Paths.log) }
    @objc private func openPrivacy() {
        NSWorkspace.shared.open(URL(string: "x-apple.systempreferences:com.apple.preference.security?Privacy_Accessibility")!)
    }
    @objc private func quit() { NSApp.terminate(nil) }

    private func alert(_ title: String, _ text: String) {
        let a = NSAlert(); a.messageText = title; a.informativeText = text; a.runModal()
    }

    // MARK: command channel (tests and diagnostics)

    private func runCommandFile() {
        let fm = FileManager.default
        guard fm.fileExists(atPath: Paths.command.path),
              let text = try? String(contentsOf: Paths.command, encoding: .utf8) else { return }
        try? fm.removeItem(at: Paths.command)
        for line in text.split(separator: "\n") { CommandChannel.run(String(line), self) }
        writeState()
    }

    func writeState() {
        let z = SkyLight.get()
        let p = CGEvent(source: nil)?.location ?? .zero
        let s: [String: Any] = [
            "started": started, "axTrusted": AXIsProcessTrusted(), "tap": mag.tapActive,
            "level": mag.level, "center": [mag.currentCenter.x, mag.currentCenter.y],
            "owner": "\(mag.viewOwner)", "locked": mag.isLocked, "inspect": mag.inspect, "paused": mag.isPaused,
            "pointer": [p.x, p.y],
            "sky": z.map { ["center": [$0.center.x, $0.center.y], "scale": $0.scale, "smooth": $0.smooth] } ?? NSNull(),
            "caret": ["kind": "\(mag.caret.snapshot.kind)", "seq": mag.caret.snapshot.seq,
                      "rect": [mag.caret.snapshot.rect.l, mag.caret.snapshot.rect.t, mag.caret.snapshot.rect.r, mag.caret.snapshot.rect.b]],
        ]
        if let d = try? JSONSerialization.data(withJSONObject: s, options: [.prettyPrinted, .sortedKeys]) {
            try? d.write(to: Paths.state)
        }
    }
}
