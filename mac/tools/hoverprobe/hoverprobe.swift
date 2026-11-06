// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
// hoverprobe banner | panel
//   banner: the first Notification Center banner (AX): "x y w h hovered" (hovered = its Close button
//           `xmark` is there, which Notification Center adds only while the pointer is over the banner).
//   panel:  a background app's non-activating panel at the bottom left with an AppKit tracking area
//           (left half) and a SwiftUI `.onHover` view (right half); prints its frame (desktop points,
//           top-left origin), then one line per event: "AK entered|exited|down", "SUI hover true|false".
// For scripts/e2e_hover.py. SwiftUI hover is what Notification Center's banners use, and it stops while
// any process hides the cursor (research/wind-mac/re/2026-11-02-notification-hover-re.md).
import AppKit
import SwiftUI
setvbuf(stdout, nil, _IONBF, 0)

func attr(_ e: AXUIElement, _ a: String) -> AnyObject? {
    var v: AnyObject?; return AXUIElementCopyAttributeValue(e, a as CFString, &v) == .success ? v : nil
}

func banner() {
    guard let nc = NSRunningApplication.runningApplications(withBundleIdentifier: "com.apple.notificationcenterui").first else { print("none"); exit(1) }
    let app = AXUIElementCreateApplication(nc.processIdentifier)
    func find(_ e: AXUIElement, _ d: Int) -> AXUIElement? {
        if (attr(e, "AXSubrole") as? String) == "AXNotificationCenterAlert" { return e }
        if d > 14 { return nil }
        for c in (attr(e, "AXChildren") as? [AXUIElement]) ?? [] { if let f = find(c, d + 1) { return f } }
        return nil
    }
    func hasClose(_ e: AXUIElement, _ d: Int) -> Bool {
        if (attr(e, "AXIdentifier") as? String) == "xmark" { return true }
        if d > 6 { return false }
        return ((attr(e, "AXChildren") as? [AXUIElement]) ?? []).contains { hasClose($0, d + 1) }
    }
    for w in (attr(app, "AXWindows") as? [AXUIElement]) ?? [] {
        guard let b = find(w, 0), let pv = attr(b, "AXPosition"), let sv = attr(b, "AXSize") else { continue }
        var p = CGPoint(), s = CGSize()
        AXValueGetValue(pv as! AXValue, .cgPoint, &p); AXValueGetValue(sv as! AXValue, .cgSize, &s)
        print(Int(p.x), Int(p.y), Int(s.width), Int(s.height), hasClose(b, 0) ? 1 : 0)
        exit(0)
    }
    print("none"); exit(1)
}

final class TV: NSView {
    var mine: NSTrackingArea?
    override func updateTrackingAreas() {
        if let t = mine { removeTrackingArea(t) }      // only ours: the tooltip's tracking area stays
        mine = NSTrackingArea(rect: bounds, options: [.mouseEnteredAndExited, .activeAlways, .inVisibleRect], owner: self)
        addTrackingArea(mine!)
    }
    override func mouseEntered(with e: NSEvent) { print("AK entered") }
    override func mouseExited(with e: NSEvent) { print("AK exited") }
    override func mouseDown(with e: NSEvent) { print("AK down") }
    override func acceptsFirstMouse(for e: NSEvent?) -> Bool { true }
    override func draw(_ r: NSRect) { NSColor.systemTeal.setFill(); r.fill() }
}
struct HV: View {
    @State var h = false
    var body: some View { Rectangle().fill(h ? Color.orange : Color.purple).onHover { v in h = v; print("SUI hover", v) } }
}

func panel() {
    let app = NSApplication.shared
    app.setActivationPolicy(.accessory)
    let at = CommandLine.arguments.dropFirst(2).compactMap { Double($0) }     // panel [x y]: bottom left
    let r = NSRect(x: at.first ?? 60, y: at.dropFirst().first ?? 60, width: 400, height: 120)
    let act = CommandLine.arguments.contains("active")
    let p = act ? NSWindow(contentRect: r, styleMask: [.titled], backing: .buffered, defer: false)
                : NSPanel(contentRect: r, styleMask: [.nonactivatingPanel, .borderless], backing: .buffered, defer: false)
    p.level = .statusBar; p.collectionBehavior = [.canJoinAllSpaces]
    p.title = "WINDTEST hover"
    let tv = TV(frame: NSRect(x: 0, y: 0, width: 200, height: 120))
    let host = NSHostingView(rootView: HV()); host.frame = NSRect(x: 200, y: 0, width: 200, height: 120)
    p.contentView = NSView(frame: NSRect(origin: .zero, size: r.size))
    p.contentView!.addSubview(tv); p.contentView!.addSubview(host)
    tv.toolTip = "WINDTEST tip"; tv.setAccessibilityElement(true); tv.setAccessibilityRole(.button); tv.setAccessibilityHelp("WINDTEST tip")                                       // AppKit tooltip: "TIP on"/"TIP off"
    var tip = false, wasActive = false, lastDump = "", lastUnder = ""
    Timer.scheduledTimer(withTimeInterval: 0.02, repeats: true) { _ in
        let pid = Int(getpid())
        let list = CGWindowListCopyWindowInfo([.optionOnScreenOnly], kCGNullWindowID) as? [[String: Any]] ?? []
        let on = list.contains { ($0[kCGWindowOwnerPID as String] as? Int) == pid && ($0[kCGWindowNumber as String] as? Int) != p.windowNumber }
        if on != tip { tip = on; print(on ? "TIP on" : "TIP off"); fflush(stdout) }
        if act {
            let all = (CGWindowListCopyWindowInfo([.optionAll], kCGNullWindowID) as? [[String: Any]] ?? []).filter { ($0[kCGWindowOwnerPID as String] as? Int) == pid }
            let d = all.map { "\($0[kCGWindowNumber as String] ?? 0)/L\($0[kCGWindowLayer as String] ?? 0)/on\($0[kCGWindowIsOnscreen as String] ?? 0)" }.joined(separator: " ")
            if d != lastDump { lastDump = d; print("WINS", d, NSApp.windows.map { "\(type(of: $0)):\($0.isVisible)" }) }
        }
        if act {
            let n = NSWindow.windowNumber(at: NSEvent.mouseLocation, belowWindowWithWindowNumber: 0)
            let u = n == p.windowNumber ? "own" : "other \(n)"
            if u != lastUnder { lastUnder = u; print("UNDER", u) }
        }
        if act, NSApp.isActive != wasActive { wasActive = NSApp.isActive; print("ACTIVE", wasActive) }
    }
    p.orderFrontRegardless()
    if CommandLine.arguments.contains("active") { app.setActivationPolicy(.regular); app.activate(ignoringOtherApps: true); p.level = .normal; p.makeKeyAndOrderFront(nil) }
    let h = NSScreen.screens[0].frame.height
    print("panel", Int(r.minX), Int(h - r.maxY), Int(r.width), Int(r.height))
    app.run()
}

switch CommandLine.arguments.dropFirst().first {
case "banner": banner()
case "panel": panel()
default: print("usage: hoverprobe banner | panel"); exit(2)
}
