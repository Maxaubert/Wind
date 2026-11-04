// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
// cursorchurn [secs]: a foreground window that sets the arrow cursor on every mouse move, the way
// Chromium does (-[RenderWidgetHostViewCocoa mouseEvent:]: research/wind-mac/2026-11-04-chatgpt-tiny-
// cursor-and-hitch-diagnosis.md). Prints its frame (desktop points, top-left origin) and, at exit, the sets.
import AppKit
setvbuf(stdout, nil, _IONBF, 0)
nonisolated(unsafe) var sets = 0
final class V: NSView {
    override func updateTrackingAreas() {
        trackingAreas.forEach(removeTrackingArea)
        addTrackingArea(NSTrackingArea(rect: bounds, options: [.mouseMoved, .activeAlways, .inVisibleRect], owner: self))
    }
    override func mouseMoved(with e: NSEvent) { NSCursor.arrow.set(); sets += 1 }
    override func draw(_ r: NSRect) {
        for i in 0..<40 { (i % 2 == 0 ? NSColor.white : NSColor.darkGray).setFill(); NSRect(x: CGFloat(i) * bounds.width / 40, y: 0, width: bounds.width / 40, height: bounds.height).fill() }
    }
}
let app = NSApplication.shared
app.setActivationPolicy(.regular)
let w = NSWindow(contentRect: NSRect(x: 20, y: 20, width: 1200, height: 800), styleMask: [.titled], backing: .buffered, defer: false)
w.title = "cursorchurn"; w.contentView = V(); w.acceptsMouseMovedEvents = true
w.makeKeyAndOrderFront(nil)
app.activate(ignoringOtherApps: true)
let f = w.frame, H = NSScreen.screens[0].frame.height
print(Int(f.minX), Int(H - f.maxY), Int(f.width), Int(f.height))
DispatchQueue.main.asyncAfter(deadline: .now() + (Double(CommandLine.arguments.dropFirst().first ?? "") ?? 10)) { print("sets", sets); exit(0) }
app.run()
