// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
import AppKit

// fakegame: a foreground app that locks the pointer the way macOS games do for mouse-look
// (hide it, disassociate it from the mouse), for scripts/e2e_mouse.py. Quits after N seconds.
let seconds = CommandLine.arguments.count > 1 ? Double(CommandLine.arguments[1]) ?? 8 : 8
let app = NSApplication.shared
app.setActivationPolicy(.regular)
let win = NSWindow(contentRect: NSRect(x: 200, y: 200, width: 400, height: 300), styleMask: [.titled], backing: .buffered, defer: false)
win.title = "fakegame"
win.makeKeyAndOrderFront(nil)
app.activate(ignoringOtherApps: true)
DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) {
    CGDisplayHideCursor(CGMainDisplayID())
    let r = CGAssociateMouseAndMouseCursorPosition(0)
    try? "locked \(r.rawValue)\n".write(toFile: NSTemporaryDirectory() + "fakegame.state", atomically: true, encoding: .utf8)
}
DispatchQueue.main.asyncAfter(deadline: .now() + seconds) {
    CGAssociateMouseAndMouseCursorPosition(1)
    CGDisplayShowCursor(CGMainDisplayID())
    exit(0)
}
app.run()
