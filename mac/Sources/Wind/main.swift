import AppKit
import WindCore

// Wind for macOS: a menu-bar agent. `Wind --watchdog <pid>` is the crash guard (see Watchdog).

if CommandLine.arguments.count == 3, CommandLine.arguments[1] == "--watchdog", let pid = pid_t(CommandLine.arguments[2]) {
    Watchdog.run(parent: pid)
}

let app = NSApplication.shared
let delegate = AppDelegate()
app.delegate = delegate
app.setActivationPolicy(.accessory)
app.run()
