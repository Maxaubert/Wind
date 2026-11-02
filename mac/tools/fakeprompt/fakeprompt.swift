import AppKit

// A window above Wind's draw overlay, standing in for a system prompt (password dialogs and the
// like sit above ordinary windows). For scripts/e2e_draw.py: `fakeprompt <seconds> [noshare]`;
// noshare opts the window out of screen capture (as password managers do).
let app = NSApplication.shared
app.setActivationPolicy(.accessory)
let secs = Double(CommandLine.arguments.dropFirst().first ?? "3") ?? 3
let w = NSWindow(contentRect: NSRect(x: 700, y: 400, width: 420, height: 180), styleMask: [.titled], backing: .buffered, defer: false)
w.title = "WINDTEST prompt"
w.level = NSWindow.Level(rawValue: NSWindow.Level.screenSaver.rawValue + 500)
if CommandLine.arguments.contains("noshare") { w.sharingType = .none }
w.orderFrontRegardless()
DispatchQueue.main.asyncAfter(deadline: .now() + secs) { app.terminate(nil) }
app.run()
