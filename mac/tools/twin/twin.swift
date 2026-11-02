// twin <title> <x> <seconds> [hide]: a plain test window app; with "hide" it hides the cursor while active (like a game)
import AppKit
let A = CommandLine.arguments
let title = A[1], x = Double(A[2])!, secs = Double(A[3])!, hide = A.count > 4
let app = NSApplication.shared
app.setActivationPolicy(.regular)
let win = NSWindow(contentRect: NSRect(x: x, y: 300, width: 500, height: 400), styleMask: [.titled], backing: .buffered, defer: false)
win.title = title
win.makeKeyAndOrderFront(nil)
app.activate(ignoringOtherApps: true)
final class D: NSObject { var hidden = false
  @objc func act(_ n: Notification) { if hide && !hidden { CGDisplayHideCursor(CGMainDisplayID()); hidden = true } }
  @objc func res(_ n: Notification) { if hidden { CGDisplayShowCursor(CGMainDisplayID()); hidden = false } } }
let d = D()
NotificationCenter.default.addObserver(d, selector: #selector(D.act(_:)), name: NSApplication.didBecomeActiveNotification, object: nil)
NotificationCenter.default.addObserver(d, selector: #selector(D.res(_:)), name: NSApplication.didResignActiveNotification, object: nil)
DispatchQueue.main.asyncAfter(deadline: .now() + secs) { exit(0) }
app.run()
