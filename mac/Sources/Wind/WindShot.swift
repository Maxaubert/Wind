// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
import AppKit
import WindCore

// Wind's own screenshot selection while zoomed (config ownScreenshot, default on; owner 2026-11-01:
// "still choppy", the crosshair and the band jump). macOS's screencapture draws its crosshair and band
// at whole desktop points, and the real cursor under zoom too, so magnified they jump `level` screen
// pixels per point, and its windows sit above anything Wind can make (research/wind-mac/re/
// 2026-11-01-drawshot-own.md). So while Wind is zoomed its event tap takes the area shortcuts itself
// (as the user has them set, com.apple.symbolichotkeys 30 and 31) and macOS's tool never starts: Wind
// draws the crosshair and the box from its own pointer model, at sub-point places every frame, with the
// engine in use (render: layers in its overlay; Transform: a click-through panel moved before each view
// write, like Wind's cursor), while the view follows the pointer as always. On release it saves the
// DESKTOP rectangle with macOS's own tool (`screencapture -p -R`, `-c` for the clipboard), so the
// user's screenshot settings apply (location, type, thumbnail, sound) and the file is the one macOS
// makes, with Wind's drawing windows kept out of it (sharingType none for the moment of the shot).
// Escape cancels, Space switches to picking a window (or, while dragging, moves the box), Option grows
// the box from its centre, Shift held while dragging keeps the edge the hand moves first (the other axis
// stays), Ctrl at the release copies instead. Cmd+Shift+5 and the colour sampler keep PointerTakeover's hand-over.

final class WindShot {
    enum Mode { case off, area, window }
    private(set) var mode = Mode.off
    var active: Bool { mode != .off }
    /// Where the press began (desktop point, the crosshair as drawn then), nil before the press.
    private(set) var anchor: CGPoint?
    /// The crosshair as drawn by the last tick (desktop point, fractional), nil before the first.
    var cross: CGPoint?
    private var clipboard = false
    /// Space held while dragging: the box moves with the pointer instead of growing.
    var moving = false
    private var lastCross: CGPoint?
    private var held = Mods()
    /// Option: the press is the box's centre.
    private var fromCentre = false
    /// Shift pressed while dragging: where the crosshair was, and the axis the hand moved first (0 x, 1 y).
    private var shiftFrom: CGPoint?, shiftAxis: Int?
    /// A wheel zoom happened while Ctrl was held (the owner's wheel zoom is Ctrl+scroll): that Ctrl is
    /// the zoom's, so releasing the button with it still down saves instead of copying.
    var ctrlZoomed = false
    /// Window mode: the window under the crosshair (id, desktop bounds).
    private(set) var picked: (id: Int, rect: CGRect)?
    private var pickBusy = false, lastPick = 0.0
    private var hotkeys: [ShotHotkey] = []
    private var hotkeysRead = 0.0
    private let q = DispatchQueue(label: "wind.shot", qos: .userInitiated)
    /// Keys whose down this mode swallowed: their up is swallowed too.
    var swallowedUps = Set<UInt16>()
    /// A shot is being written: Wind's drawing windows are out of captures until it is.
    private(set) var saving = false

    /// The user's area shortcuts (re-read every few seconds: System Settings may change them).
    func hotkey(code: UInt16, mods: Mods) -> ShotHotkey? {
        let now = CACurrentMediaTime()
        if now - hotkeysRead > 5 {
            hotkeysRead = now
            CFPreferencesAppSynchronize("com.apple.symbolichotkeys" as CFString)   // a change in System Settings, not a cached copy
            let t = CFPreferencesCopyAppValue("AppleSymbolicHotKeys" as CFString, "com.apple.symbolichotkeys" as CFString) as? [String: Any]
            hotkeys = screenshotAreaHotkeys(t)
        }
        return hotkeys.first { $0.keyCode == code && $0.mods == mods }
    }

    /// macOS's own area selection started from its shortcut (`screencapture -pdin -z keyboard.selection`,
    /// `.clipboard` for the copy one, measured 2026-11-02): its pid and whether it copies. Started at 1x it
    /// is macOS's; zooming in then swaps it for Wind's (owner: started unzoomed, the crosshair wobbled).
    static func nativeHotkeySelection() -> (pid: pid_t, clipboard: Bool)? {
        var n = proc_listallpids(nil, 0)
        guard n > 0 else { return nil }
        var pids = [pid_t](repeating: 0, count: Int(n) + 32)
        n = proc_listallpids(&pids, Int32(pids.count * MemoryLayout<pid_t>.size))
        var path = [CChar](repeating: 0, count: 4096)
        for pid in pids.prefix(Int(max(n, 0))) where pid > 0 {
            guard proc_name(pid, &path, UInt32(path.count)) > 0, String(cString: path) == "screencapture" else { continue }
            guard let a = arguments(pid), let z = a.firstIndex(of: "-z"), z + 1 < a.count,
                  a[z + 1].hasPrefix("keyboard.selection") else { continue }
            return (pid, a[z + 1].hasSuffix(".clipboard"))
        }
        return nil
    }

    private static func arguments(_ pid: pid_t) -> [String]? {
        var mib = [CTL_KERN, KERN_PROCARGS2, pid]
        var size = 0
        guard sysctl(&mib, 3, nil, &size, nil, 0) == 0, size > 4 else { return nil }
        var buf = [UInt8](repeating: 0, count: size)
        guard sysctl(&mib, 3, &buf, &size, nil, 0) == 0 else { return nil }
        let argc = buf.withUnsafeBytes { $0.load(as: Int32.self) }
        // argc, the executable path, padding NULs, then argc NUL-terminated arguments.
        var i = 4
        while i < size && buf[i] != 0 { i += 1 }
        while i < size && buf[i] == 0 { i += 1 }
        var out: [String] = []
        while out.count < argc && i < size {
            let start = i
            while i < size && buf[i] != 0 { i += 1 }
            out.append(String(decoding: buf[start..<i], as: UTF8.self))
            i += 1
        }
        return out
    }

    func begin(clipboard c: Bool) {
        mode = .area; anchor = nil; clipboard = c; moving = false; picked = nil; lastCross = nil
        fromCentre = false; shiftFrom = nil; shiftAxis = nil; ctrlZoomed = false
        held = Mods(rawValue: CGEventSource.flagsState(.combinedSessionState).rawValue).intersection(.all)  // the hotkey's own Shift is no lock
        Log.info("own screenshot selection\(c ? " (to the clipboard)" : "")")
    }

    func cancel(_ why: String) {
        guard active else { return }
        mode = .off; anchor = nil; moving = false; picked = nil
        Log.info("own screenshot selection cancelled (\(why))")
    }

    /// Space: area <-> window before the press; while dragging it moves the box (held).
    func space(down: Bool) {
        if anchor != nil { moving = down; return }
        guard down else { return }
        mode = mode == .area ? .window : .area
        picked = nil; lastPick = 0
    }

    func press() {
        guard mode == .area, let c = cross else { return }
        anchor = c
    }

    /// Modifier changes from the tap: Option and Shift act on their press (edges, as in macOS's mode).
    func modifiers(_ m: Mods) {
        if !m.contains(.ctrl) { ctrlZoomed = false }
        if m.contains(.opt) != held.contains(.opt) { fromCentre = m.contains(.opt) }
        if m.contains(.shift) != held.contains(.shift) {
            shiftFrom = m.contains(.shift) && anchor != nil ? cross : nil; shiftAxis = nil
        }
        held = m
    }

    /// Called every tick with the crosshair as drawn: the box moves with Space held.
    func track(_ c: CGPoint) {
        if moving, let a = anchor, let l = lastCross { anchor = CGPoint(x: a.x + c.x - l.x, y: a.y + c.y - l.y) }
        lastCross = c; cross = c
        if let f = shiftFrom, shiftAxis == nil, max(abs(c.x - f.x), abs(c.y - f.y)) >= 1 {
            shiftAxis = abs(c.x - f.x) >= abs(c.y - f.y) ? 0 : 1
        }
        if mode == .window { pickWindow(at: c) }
    }

    /// The box's far corner: the crosshair, with Shift's still axis held.
    private var end: CGPoint? {
        guard let c = cross else { return nil }
        guard let f = shiftFrom, let ax = shiftAxis else { return c }
        return ax == 0 ? CGPoint(x: c.x, y: f.y) : CGPoint(x: f.x, y: c.y)
    }

    /// What the selection draws, as it is now (a value: render on xBR draws the one of the tick whose
    /// picture is on screen, a refresh behind).
    var geom: ShotGeom? {
        guard active, let c = cross else { return nil }
        return ShotGeom(mode: mode, cross: c, box: box, anchor: anchor, picked: picked?.rect)
    }

    /// The box as drawn (desktop points), nil before the press.
    var box: CGRect? {
        guard let a = anchor, let e = end else { return nil }
        let dx = abs(e.x - a.x), dy = abs(e.y - a.y)
        if fromCentre { return CGRect(x: a.x - dx, y: a.y - dy, width: 2 * dx, height: 2 * dy) }
        return CGRect(x: min(a.x, e.x), y: min(a.y, e.y), width: dx, height: dy)
    }

    /// Button up: save what is selected (Ctrl held copies), then the mode ends.
    func release(ctrl: Bool, screen: Sz) {
        let toClip = clipboard || (ctrl && !ctrlZoomed)
        defer { mode = .off; anchor = nil; moving = false; picked = nil; shiftFrom = nil; fromCentre = false }
        if mode == .window {
            guard let p = picked else { Log.info("own screenshot: no window under the pointer"); return }
            var args = ["-l", String(p.id)]
            if (CFPreferencesCopyAppValue("disable-shadow" as CFString, "com.apple.screencapture" as CFString) as? Bool) == true { args.append("-o") }
            save(args, clipboard: toClip, what: "window \(p.id)")
            return
        }
        guard let b = box, let r = shotRect(anchor: Pt(b.minX, b.minY), end: Pt(b.maxX, b.maxY), screen: screen) else {
            Log.info("own screenshot: no area (a click)"); return
        }
        save(["-R", "\(r.x),\(r.y),\(r.w),\(r.h)"], clipboard: toClip,
             what: String(format: "%d,%d %dx%d (drawn %.2f,%.2f to %.2f,%.2f)", r.x, r.y, r.w, r.h, b.minX, b.minY, b.maxX, b.maxY))
    }

    /// macOS's own tool writes the shot: `-p` takes the user's settings (location-screenshot, type,
    /// show-thumbnail, target; measured 2026-11-01), `-c` the clipboard. Wind's click-through drawing
    /// windows (render's overlay, Wind's cursor, the selection) are kept out of it with sharingType none
    /// (screencapture -R leaves such windows out, measured), restored once the tool has exited.
    private func save(_ what: [String], clipboard: Bool, what desc: String) {
        var args = clipboard ? ["-c"] : ["-p"]
        args += what
        #if WIND_TEST_CHANNEL
        if ProcessInfo.processInfo.environment["WIND_SHOT_QUIET"] != nil { args.insert("-x", at: 0) }   // tests: no shutter sound
        #endif
        // Hidden ones too: Wind's cursor comes back on the tick after the release, while the tool is still
        // capturing (it was in Transform's shots).
        let mine = NSApp.windows.filter { $0.ignoresMouseEvents && $0.sharingType != .none }
        let was = mine.map(\.sharingType)
        for w in mine { w.sharingType = .none }
        func back() { for (w, t) in zip(mine, was) { w.sharingType = t } }
        saving = true
        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/usr/sbin/screencapture")
        p.arguments = args
        let t0 = CACurrentMediaTime()
        p.terminationHandler = { [weak self] pr in
            DispatchQueue.main.async {
                back()
                self?.saving = false
                Log.info(String(format: "own screenshot saved: %@ (screencapture %@, exit %d, %.0f ms)", desc, args.joined(separator: " "), pr.terminationStatus, (CACurrentMediaTime() - t0) * 1000))
            }
        }
        do { try p.run() } catch {
            back()
            saving = false
            Log.error("own screenshot: screencapture did not start: \(error.localizedDescription)")
        }
    }

    /// Window mode: the topmost ordinary window under the crosshair (off the main thread, 20 times a
    /// second at most: the window list takes a few ms).
    private func pickWindow(at c: CGPoint) {
        let now = CACurrentMediaTime()
        guard !pickBusy, now - lastPick > 0.05 else { return }
        pickBusy = true; lastPick = now
        let me = ProcessInfo.processInfo.processIdentifier
        q.async { [weak self] in
            var hit: (Int, CGRect)?
            let list = CGWindowListCopyWindowInfo([.optionOnScreenOnly, .excludeDesktopElements], kCGNullWindowID) as? [[String: Any]] ?? []
            for w in list {
                let layer = w[kCGWindowLayer as String] as? Int ?? 0
                guard (w[kCGWindowOwnerPID as String] as? Int32) != me, layer >= 0, layer < 20,
                      (w[kCGWindowAlpha as String] as? Double ?? 1) > 0.05,
                      let d = w[kCGWindowBounds as String] as? NSDictionary, let b = CGRect(dictionaryRepresentation: d),
                      b.width >= 20, b.height >= 20, b.contains(c) else { continue }
                hit = (w[kCGWindowNumber as String] as? Int ?? 0, b); break
            }
            DispatchQueue.main.async {
                guard let self else { return }
                self.pickBusy = false
                if self.mode == .window { self.picked = hit.map { (id: $0.0, rect: $0.1) } }
            }
        }
    }

}

/// The drawing (crosshair, box, window highlight; no coordinate label, as macOS's own at 1x has none, owner 2026-11-02) as layers in a host layer: render's overlay
/// (screen points) or Transform's panel (desktop points, magnified by the zoom). Sizes are given in
/// desktop points and drawn magnified, as macOS's own crosshair looks under zoom.
/// One frame of the selection's drawing (desktop points).
struct ShotGeom {
    var mode: WindShot.Mode
    var cross: CGPoint
    var box: CGRect?
    var anchor: CGPoint?
    var picked: CGRect?
}

final class ShotLayers {
    let root = CALayer()
    private let box = CAShapeLayer(), boxDark = CAShapeLayer()
    private let crossRoot = CALayer(), crossOutline = CAShapeLayer(), crossLine = CAShapeLayer()
    private let pick = CALayer()
    private let probeCross = CALayer(), probeFar = CALayer(), probeAnchor = CALayer()
    private var geom: (Double, Double) = (0, 0)
    #if WIND_TEST_CHANNEL
    /// Test builds: magenta at the crosshair, yellow 20 px right and below the box's far corner, cyan
    /// on the anchor corner, for the zoomed-screen capture (tools/probecap).
    static let probe = ProcessInfo.processInfo.environment["WIND_CURSOR_PROBE"] != nil
    #else
    static let probe = false
    #endif

    init() {
        root.bounds = .zero; root.anchorPoint = .zero; root.position = .zero
        box.fillColor = CGColor(gray: 1, alpha: 0.16)
        boxDark.fillColor = nil; boxDark.strokeColor = CGColor(gray: 0, alpha: 0.55)
        box.strokeColor = CGColor(gray: 1, alpha: 0.95)
        pick.backgroundColor = NSColor.systemBlue.withAlphaComponent(0.28).cgColor
        crossOutline.strokeColor = CGColor(gray: 0, alpha: 0.7); crossLine.strokeColor = CGColor(gray: 1, alpha: 1)
        for l in [crossOutline, crossLine] { l.fillColor = nil; l.lineCap = .butt; crossRoot.addSublayer(l) }
        crossRoot.bounds = .zero
        for l in [pick, boxDark, box, crossRoot] as [CALayer] { root.addSublayer(l) }
        // No implicit animations at all: every place is set each frame.
        let still: [String: CAAction] = ["contents": NSNull(), "string": NSNull(), "bounds": NSNull(), "position": NSNull(),
                                         "frame": NSNull(), "hidden": NSNull(), "path": NSNull(), "cornerRadius": NSNull()]
        for l in [root, pick, boxDark, box, crossRoot, crossOutline, crossLine, probeCross, probeFar, probeAnchor] as [CALayer] { l.actions = still }
        if ShotLayers.probe {
            probeCross.backgroundColor = CGColor(srgbRed: 1, green: 0, blue: 1, alpha: 1)
            probeFar.backgroundColor = CGColor(srgbRed: 1, green: 1, blue: 0, alpha: 1)
            probeAnchor.backgroundColor = CGColor(srgbRed: 0, green: 1, blue: 1, alpha: 1)
            crossRoot.insertSublayer(probeCross, at: 0)
            for l in [probeFar, probeAnchor] { root.addSublayer(l) }
        }
        root.isHidden = true
    }

    /// Place everything (inside the caller's no-animation transaction). `map` takes a desktop point to
    /// the host's coordinates (bottom-left origin); `mag` is host units per desktop point, `px` per
    /// screen point.
    func update(_ s: ShotGeom?, map: (CGPoint) -> CGPoint, mag: Double, px: Double) {
        guard let s else { root.isHidden = true; return }
        let c = s.cross
        root.isHidden = false
        if geom != (mag, px) {
            geom = (mag, px)
            let a = 10.0 * mag, g = 2.5 * mag                    // arms and the gap at the centre, desktop pt
            let path = CGMutablePath()
            path.move(to: CGPoint(x: -a, y: 0)); path.addLine(to: CGPoint(x: -g, y: 0))
            path.move(to: CGPoint(x: g, y: 0)); path.addLine(to: CGPoint(x: a, y: 0))
            path.move(to: CGPoint(x: 0, y: -a)); path.addLine(to: CGPoint(x: 0, y: -g))
            path.move(to: CGPoint(x: 0, y: g)); path.addLine(to: CGPoint(x: 0, y: a))
            crossLine.path = path; crossLine.lineWidth = 0.75 * mag
            crossOutline.path = path; crossOutline.lineWidth = 0.75 * mag + 2 * px
            box.lineWidth = 1 * px; boxDark.lineWidth = 3 * px
            for (l, sz) in [(probeCross, 12.0), (probeFar, 12.0), (probeAnchor, 12.0)] { l.bounds = CGRect(x: 0, y: 0, width: sz * px, height: sz * px) }
            probeCross.position = .zero
        }
        let cp = map(c)
        crossRoot.position = cp
        crossRoot.isHidden = s.mode == .window && s.picked != nil
        if let b = s.box, s.mode == .area {
            let p0 = map(CGPoint(x: b.minX, y: b.minY)), p1 = map(CGPoint(x: b.maxX, y: b.maxY))
            let r = CGRect(x: min(p0.x, p1.x), y: min(p0.y, p1.y), width: abs(p1.x - p0.x), height: abs(p1.y - p0.y))
            box.path = CGPath(rect: r, transform: nil); boxDark.path = box.path
            box.isHidden = false; boxDark.isHidden = false
            if ShotLayers.probe, let a = s.anchor {
                probeAnchor.isHidden = false; probeAnchor.position = map(a)
                probeFar.isHidden = false; probeFar.position = CGPoint(x: cp.x + 20 * px, y: cp.y - 20 * px)
            }
        } else {
            box.isHidden = true; boxDark.isHidden = true; probeAnchor.isHidden = true; probeFar.isHidden = true
        }
        if s.mode == .window, let p = s.picked {
            let p0 = map(CGPoint(x: p.minX, y: p.minY)), p1 = map(CGPoint(x: p.maxX, y: p.maxY))
            pick.frame = CGRect(x: min(p0.x, p1.x), y: min(p0.y, p1.y), width: abs(p1.x - p0.x), height: abs(p1.y - p0.y))
            pick.isHidden = false
        } else { pick.isHidden = true }
    }
}

/// Transform: the selection in a click-through panel that covers the screen and never moves (a moved
/// window reached the screen 1-2 frames after the zoom write); the zoom magnifies it. Its crosshair
/// moves before every view write, as Wind's cursor does, so both reach the same frame.
final class ShotPanel {
    private var panel: NSPanel?
    private let layers = ShotLayers()
    private var level = 1.0
    private weak var shot: WindShot?

    func show(_ s: WindShot?, level lv: Double) {
        guard let s, s.active, s.cross != nil else { panel?.orderOut(nil); return }
        shot = s; level = max(1, lv)
        let panel = self.panel ?? make()
        place(panel)
        if !panel.isVisible { panel.orderFrontRegardless() }
        CATransaction.flush()
    }

    /// Just before a view write: the crosshair (and the box's far corner) to where that write shows it.
    func move(cross c: CGPoint) {
        guard let panel, panel.isVisible, let s = shot, s.active else { return }
        s.cross = c
        place(panel)
        CATransaction.flush()
    }

    var visible: Bool { panel?.isVisible ?? false }

    private func place(_ panel: NSPanel) {
        let primaryH = NSScreen.screens.first?.frame.height ?? 0
        let f = NSScreen.screens.first?.frame ?? NSRect(x: 0, y: 0, width: 1, height: 1)
        if panel.frame != f { panel.setFrame(f, display: false) }
        CATransaction.begin(); CATransaction.setDisableActions(true)
        let top = primaryH - f.maxY
        layers.update(shot?.geom, map: { CGPoint(x: $0.x - f.minX, y: f.height - ($0.y - top)) }, mag: 1, px: 1 / level)
        CATransaction.commit()
    }

    private func make() -> NSPanel {
        let p = NSPanel(contentRect: .zero, styleMask: [.borderless, .nonactivatingPanel], backing: .buffered, defer: false)
        p.isOpaque = false
        p.backgroundColor = .clear
        p.hasShadow = false
        p.animationBehavior = .none
        p.ignoresMouseEvents = true
        p.level = NSWindow.Level(rawValue: NSWindow.Level.screenSaver.rawValue + 1)
        p.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary, .stationary, .ignoresCycle]
        let v = NSView()
        v.wantsLayer = true
        v.layer?.addSublayer(layers.root)
        p.contentView = v
        SkyLight.ignoreForWindowPicker(windowNumber: p.windowNumber)
        panel = p
        return p
    }
}
