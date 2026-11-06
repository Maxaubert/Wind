// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
// cursorchurn [secs]: a foreground window that sets the arrow cursor on every mouse move, the way
// Chromium does (-[RenderWidgetHostViewCocoa mouseEvent:]: research/wind-mac/2026-11-04-chatgpt-tiny-
// cursor-and-hitch-diagnosis.md). `flash` as the second argument inverts the stripes every 0.4 s. Prints its frame (desktop points, top-left origin) and, at exit, the sets.
import AppKit
setvbuf(stdout, nil, _IONBF, 0)
nonisolated(unsafe) var sets = 0
nonisolated(unsafe) var inverted = false
final class V: NSView {
    override func updateTrackingAreas() {
        trackingAreas.forEach(removeTrackingArea)
        addTrackingArea(NSTrackingArea(rect: bounds, options: [.mouseMoved, .activeAlways, .inVisibleRect], owner: self))
    }
    // hover: like a page's hover effect, the pointing hand over white stripes and the arrow over grey ones,
    // set only when it changes (owner 2026-11-04, a file label in a browser).
    var hand = false
    override func mouseMoved(with e: NSEvent) {
        if CommandLine.arguments.contains("hover") {
            let i = Int(convert(e.locationInWindow, from: nil).x / (bounds.width / 40))
            let want = i % 2 == 0
            if want != hand { hand = want; (want ? NSCursor.pointingHand : NSCursor.arrow).set(); sets += 1 }
            return
        }
        NSCursor.arrow.set(); sets += 1
    }
    override func draw(_ r: NSRect) {
        if solid { (inverted ? NSColor.white : NSColor.black).setFill(); bounds.fill(); return }
        if CommandLine.arguments.contains("grey") { NSColor(white: 0.5, alpha: 1).setFill(); bounds.fill(); return }
        for i in 0..<40 { ((i % 2 == 0) != inverted ? NSColor.white : NSColor.darkGray).setFill(); NSRect(x: CGFloat(i) * bounds.width / 40, y: 0, width: bounds.width / 40, height: bounds.height).fill() }
    }
}
let app = NSApplication.shared
app.setActivationPolicy(.regular)
let w = NSWindow(contentRect: NSRect(x: 20, y: 20, width: 1200, height: 800), styleMask: [.titled], backing: .buffered, defer: false)
w.title = "cursorchurn"; w.contentView = V(); w.acceptsMouseMovedEvents = true
w.makeKeyAndOrderFront(nil)
app.activate(ignoringOtherApps: true)
// solid: the whole window black, then white, 30 times a second: a frame shown whole has every spot agreeing.
nonisolated(unsafe) var solid = false
if CommandLine.arguments.contains("solid") {
    solid = true
    Timer.scheduledTimer(withTimeInterval: 0.033, repeats: true) { _ in inverted.toggle(); w.contentView?.needsDisplay = true }
}
if CommandLine.arguments.contains("flash") {
    Timer.scheduledTimer(withTimeInterval: 0.4, repeats: true) { _ in inverted.toggle(); w.contentView?.needsDisplay = true }
}
let f = w.frame, H = NSScreen.screens[0].frame.height
print(Int(f.minX), Int(H - f.maxY), Int(f.width), Int(f.height))
DispatchQueue.main.asyncAfter(deadline: .now() + (Double(CommandLine.arguments.dropFirst().first ?? "") ?? 10)) { print("sets", sets); exit(0) }
app.run()
