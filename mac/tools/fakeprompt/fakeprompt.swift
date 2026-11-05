// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
import AppKit

// A window above Wind's draw overlay, standing in for a system prompt (password dialogs and the
// like sit above ordinary windows). For scripts/e2e_draw.py: `fakeprompt <seconds> [noshare|alert]`;
// noshare opts the window out of screen capture (as password managers do).
// alert: a real system alert instead, shown by UserNotificationCenter (CFUserNotification, the way
// permission prompts and other system alerts come up); it sits on a space above Wind's cursor panel on
// Transform (render's overlay space is above it). It closes itself after <seconds> (its timeout), or on SIGTERM (cancelled), and prints
// "clicked" when its OK button was pressed, else "closed".
let args = CommandLine.arguments
let secs = Double(args.dropFirst().first ?? "3") ?? 3
if args.contains("alert") {
    var err: Int32 = 0
    let d: [String: Any] = [kCFUserNotificationAlertHeaderKey as String: "WINDTEST prompt",
                            kCFUserNotificationAlertMessageKey as String: "Wind test alert. It closes by itself.",
                            kCFUserNotificationDefaultButtonTitleKey as String: "OK"]
    guard let n = CFUserNotificationCreate(nil, secs, CFOptionFlags(kCFUserNotificationNoteAlertLevel), &err, d as CFDictionary) else { exit(1) }
    signal(SIGTERM, SIG_IGN)
    let s = DispatchSource.makeSignalSource(signal: SIGTERM, queue: .global())
    s.setEventHandler { CFUserNotificationCancel(n); exit(0) }
    s.resume()
    var r: CFOptionFlags = 0
    let got = CFUserNotificationReceiveResponse(n, secs + 1, &r)
    // Which way it closed, for tests that click its button: "clicked" (OK) or "closed" (timed out).
    print(got == 0 && r & 3 == CFOptionFlags(kCFUserNotificationDefaultResponse) ? "clicked" : "closed"); fflush(stdout)
    exit(0)
}
let app = NSApplication.shared
app.setActivationPolicy(.accessory)
let w = NSWindow(contentRect: NSRect(x: 700, y: 400, width: 420, height: 180), styleMask: [.titled], backing: .buffered, defer: false)
w.title = "WINDTEST prompt"
w.level = NSWindow.Level(rawValue: NSWindow.Level.screenSaver.rawValue + 500)
if args.contains("noshare") { w.sharingType = .none }
w.orderFrontRegardless()
DispatchQueue.main.asyncAfter(deadline: .now() + secs) { app.terminate(nil) }
app.run()
