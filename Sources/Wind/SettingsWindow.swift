import AppKit
import ServiceManagement
import SwiftUI
import WindCore

// The Settings window. Every control edits the same config.json the menu and a text editor do;
// changes apply immediately (the file is written and Wind reloads it).

final class SettingsModel: ObservableObject {
    @Published var config: Config
    @Published var zoomShortcuts: [String] = []
    @Published var shakeOn = false
    @Published var loginOn = false
    let save: (Config) -> Void
    let fixZoom: () -> Void

    init(config: Config, save: @escaping (Config) -> Void, fixZoom: @escaping () -> Void) {
        self.config = config
        self.save = save
        self.fixZoom = fixZoom
        refreshSystem()
    }

    func refreshSystem() {
        zoomShortcuts = SystemZoom.shortcutsOn()
        shakeOn = SystemZoom.shakeToLocateOn
        loginOn = SMAppService.mainApp.status == .enabled
    }

    /// A binding into the config that saves on every change.
    func bind<T>(_ kp: WritableKeyPath<Config, T>) -> Binding<T> {
        Binding(get: { self.config[keyPath: kp] }, set: { v in
            self.config[keyPath: kp] = v
            self.config.clamp()
            self.save(self.config)
        })
    }

    /// Binds as one comma-separated line per action.
    func bindText(_ kp: WritableKeyPath<Config, [String]>) -> Binding<String> {
        Binding(get: { self.config[keyPath: kp].joined(separator: ", ") }, set: { v in
            self.config[keyPath: kp] = v.split(separator: ",").map { $0.trimmingCharacters(in: .whitespaces) }.filter { !$0.isEmpty }
            self.save(self.config)
        })
    }
}

struct SettingsView: View {
    @ObservedObject var m: SettingsModel

    var body: some View {
        TabView {
            zoomTab.tabItem { Text("Zoom") }
            mouseTab.tabItem { Text("Mouse") }
            trackingTab.tabItem { Text("Tracking") }
            bindsTab.tabItem { Text("Binds") }
            macTab.tabItem { Text("macOS") }
        }
        .padding(16)
        .frame(width: 520, height: 430)
    }

    private func slider(_ title: String, _ kp: WritableKeyPath<Config, Double>, _ range: ClosedRange<Double>, step: Double, format: String) -> some View {
        HStack {
            Text(title).frame(width: 170, alignment: .leading)
            Slider(value: m.bind(kp), in: range, step: step)
            Text(String(format: format, m.config[keyPath: kp])).monospacedDigit().frame(width: 56, alignment: .trailing)
        }
    }

    private var zoomTab: some View {
        Form {
            slider("Zoom-in speed", \.zoomInSpeed, 0.25...4, step: 0.05, format: "%.2f")
            slider("Zoom-out speed", \.zoomOutSpeed, 0.25...4, step: 0.05, format: "%.2f")
            slider("Wheel zoom speed", \.wheelZoomSpeed, 0.1...3, step: 0.05, format: "%.2f")
            slider("Maximum zoom", \.maxLevel, 2...64, step: 1, format: "%.0fx")
            slider("Quick zoom level", \.quickZoomDefault, 1.5...16, step: 0.5, format: "%.1fx")
            slider("Release ease-out", \.zoomEaseOutMs, 0...400, step: 5, format: "%.0f ms")
            Toggle("Smooth start when zooming in", isOn: m.bind(\.smoothZoom))
            Toggle("Smooth pixels (off = sharp)", isOn: m.bind(\.smoothImages))
            Divider()
            slider("Warmth", \.warmthPct, 0...100, step: 5, format: "%.0f%%")
            slider("Brightness", \.dimPct, 10...100, step: 5, format: "%.0f%%")
        }
    }

    private var mouseTab: some View {
        Form {
            Picker("The view follows the pointer", selection: m.bind(\.mouseAlign)) {
                Text("Centred").tag(0)
                Text("Within the edges").tag(1)
            }
            if m.config.mouseAlign == 1 {
                slider("Edge margin", \.mouseMarginPct, 0...40, step: 1, format: "%.0f%%")
            }
            Toggle("Keep pointer speed when zoomed", isOn: m.bind(\.keepPointerSpeed))
            Text("macOS slows the pointer by the zoom level; Wind keeps the speed you have at 1x.")
                .font(.caption).foregroundStyle(.secondary)
            Toggle("Follow the mouse when a game locks the pointer", isOn: m.bind(\.lockTracking))
            slider("Game and Inspect sensitivity", \.cursorSensitivity, 0.25...4, step: 0.05, format: "%.2f")
            slider("Keyboard pan speed", \.panSpeed, 0.25...4, step: 0.05, format: "%.2f")
        }
    }

    private var trackingTab: some View {
        Form {
            Toggle("Follow the text caret", isOn: m.bind(\.trackCaret))
            Toggle("Follow keyboard focus", isOn: m.bind(\.trackFocus))
            Picker("Show the caret", selection: m.bind(\.trackAlign)) {
                Text("Centred").tag(0)
                Text("Within the edges").tag(1)
            }
            slider("Glide time", \.trackGlideMs, 0...600, step: 10, format: "%.0f ms")
            if m.config.trackAlign == 1 {
                slider("Edge margin", \.trackMarginPct, 0...40, step: 1, format: "%.0f%%")
            }
        }
    }

    private var bindsTab: some View {
        let problems = m.config.binds().problems
        return Form {
            TextField("Zoom in (hold)", text: m.bindText(\.zoomIn))
            TextField("Zoom out (hold)", text: m.bindText(\.zoomOut))
            TextField("Wheel zoom modifiers", text: m.bind(\.wheelZoomMods))
            TextField("Quick zoom", text: m.bindText(\.quickZoom))
            TextField("Pan left", text: m.bindText(\.panLeft))
            TextField("Pan right", text: m.bindText(\.panRight))
            TextField("Pan up", text: m.bindText(\.panUp))
            TextField("Pan down", text: m.bindText(\.panDown))
            TextField("Inspect", text: m.bindText(\.inspect))
            TextField("Quit Wind", text: m.bindText(\.quit))
            Text("Comma-separated, like \"mouse4, ctrl+opt+equal\". Modifiers: ctrl, opt, shift, cmd. Mouse buttons: mouse3 (back), mouse4 (forward), mouse2 (middle).")
                .font(.caption).foregroundStyle(.secondary)
            ForEach(problems, id: \.self) { p in
                Text("⚠︎ \(p)").font(.caption).foregroundStyle(.orange)
            }
        }
    }

    private var macTab: some View {
        Form {
            if m.zoomShortcuts.isEmpty {
                Label("macOS Zoom shortcuts are off", systemImage: "checkmark.circle")
            } else {
                Label("macOS Zoom shortcuts are on and fight Wind: \(m.zoomShortcuts.joined(separator: ", "))", systemImage: "exclamationmark.triangle")
                Button("Turn off macOS Zoom shortcuts") { m.fixZoom() }
            }
            Toggle("Enlarge the pointer when it is shaken (macOS)", isOn: Binding(get: { m.shakeOn }, set: { on in
                SystemZoom.setShakeToLocate(on) { _ in DispatchQueue.main.async { m.refreshSystem() } }
            }))
            Toggle("Open Wind at login", isOn: Binding(get: { m.loginOn }, set: { on in
                let svc = SMAppService.mainApp
                do { if on { try svc.register() } else { try svc.unregister() } }
                catch { Log.error("open at login: \(error.localizedDescription)") }
                m.refreshSystem()
            }))
            Divider()
            Button("Open the settings file") { NSWorkspace.shared.open(Paths.config) }
            Button("Show the log") { NSWorkspace.shared.open(Paths.log) }
        }
        .onAppear { m.refreshSystem() }
    }
}

final class SettingsWindowController {
    private var window: NSWindow?
    private var model: SettingsModel?

    func show(config: Config, save: @escaping (Config) -> Void, fixZoom: @escaping () -> Void) {
        if let w = window, let m = model {
            m.config = config
            m.refreshSystem()
            NSApp.activate(ignoringOtherApps: true)
            w.makeKeyAndOrderFront(nil)
            return
        }
        let m = SettingsModel(config: config, save: save, fixZoom: fixZoom)
        let w = NSWindow(contentViewController: NSHostingController(rootView: SettingsView(m: m)))
        w.title = "Wind Settings"
        w.styleMask = [.titled, .closable, .miniaturizable]
        w.isReleasedWhenClosed = false
        w.center()
        model = m
        window = w
        NSApp.activate(ignoringOtherApps: true)
        w.makeKeyAndOrderFront(nil)
    }

    /// Keep an open window in step with config changes made elsewhere (menu, text editor).
    func update(_ c: Config) {
        if let m = model, m.config != c { m.config = c }
    }
}
