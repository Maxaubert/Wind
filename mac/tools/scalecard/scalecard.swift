// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
// scalecard card <x> <y> <w> <h> <seconds>   a test card window at that desktop rect (points, top-left
//                                            origin): text light and dark, thin lines, a 1 px checker,
//                                            circles, a button, noise. Prints "ready" once it is on screen.
// scalecard grab <out.ppm> [x y w h]         one frame of what is on screen, at the backing resolution,
//                                            cropped to that rect (pixels): this process has no window and
//                                            leaves only itself out, so Render's overlay is in it.
// scalecard patch <x> <y> <w> <h>            a plain window at that rect; stdin lines "r g b" (0-255) set its colour,
//                                            "move x y" moves it; prints "ready", then "ok" after each line.
// For the Render scaling checks in scripts/e2e_draw.py (tools/upscaleref computes what each filter
// should show from a grab at 1x).
import AppKit
import ScreenCaptureKit
import CoreMedia

let a = CommandLine.arguments
setvbuf(stdout, nil, _IOLBF, 0)

final class Card: NSView {
    override var isFlipped: Bool { true }
    override func draw(_ r: NSRect) {
        let w = bounds.width / 2, h = bounds.height / 2
        NSColor.white.setFill(); NSRect(x: 0, y: 0, width: bounds.width, height: bounds.height).fill()
        let text = "Wind 0123456789 il1| O0 rn m {}[] The quick brown fox jumps over the lazy dog."
        var y: CGFloat = 3
        for sz in [9.0, 11.0, 13.0, 16.0] {
            NSAttributedString(string: text, attributes: [.font: NSFont.systemFont(ofSize: sz), .foregroundColor: NSColor.black])
                .draw(with: NSRect(x: 4, y: y, width: w - 8, height: sz * 2.5), options: [.usesLineFragmentOrigin]); y += sz * 2.5
        }
        NSColor(white: 0.12, alpha: 1).setFill(); NSRect(x: w, y: h, width: w, height: h).fill()
        y = h + 3
        for sz in [11.0, 13.0] {
            NSAttributedString(string: text, attributes: [.font: NSFont.systemFont(ofSize: sz), .foregroundColor: NSColor(white: 0.92, alpha: 1)])
                .draw(with: NSRect(x: w + 4, y: y, width: w - 8, height: sz * 3.4), options: [.usesLineFragmentOrigin]); y += sz * 3.4
        }
        NSColor.black.setStroke()
        let o = NSPoint(x: w + 4, y: h - 4)
        for k in 0..<14 {
            let ang = Double(k) * (Double.pi / 2) / 13.0
            let p = NSBezierPath(); p.move(to: o); p.line(to: NSPoint(x: o.x + w * cos(ang), y: o.y - w * sin(ang)))
            p.lineWidth = k % 2 == 0 ? 0.5 : 1; p.stroke()
        }
        NSColor.systemRed.setStroke()
        for rr in stride(from: 16.0, through: 70.0, by: 9.0) { let c = NSBezierPath(ovalIn: NSRect(x: w * 1.6 - rr / 2, y: h * 0.35 - rr / 2, width: rr, height: rr)); c.lineWidth = 0.5; c.stroke() }
        NSColor.black.setFill()
        for yy in 0..<24 { for xx in 0..<48 where (xx + yy) % 2 == 0 { NSRect(x: w + 10 + CGFloat(xx) * 0.5, y: 6 + CGFloat(yy) * 0.5, width: 0.5, height: 0.5).fill() } }
        let b = NSBezierPath(roundedRect: NSRect(x: 8, y: h + 10, width: 80, height: 22), xRadius: 6, yRadius: 6)
        NSColor.systemBlue.setFill(); b.fill()
        NSAttributedString(string: "Button", attributes: [.font: NSFont.systemFont(ofSize: 13), .foregroundColor: NSColor.white]).draw(at: NSPoint(x: 26, y: h + 13))
        var seed: UInt32 = 7
        for yy in stride(from: h + 40, to: bounds.height - 4, by: 0.5) {
            for xx in stride(from: 8.0, to: w - 8, by: 0.5) {
                seed = seed &* 1664525 &+ 1013904223
                NSColor(srgbRed: CGFloat(seed >> 24) / 255, green: CGFloat((seed >> 16) & 255) / 255, blue: 0.4, alpha: 1).setFill()
                NSRect(x: xx, y: yy, width: 0.5, height: 0.5).fill()
            }
        }
    }
}

func card() {
    let x = Double(a[2])!, y = Double(a[3])!, w = Double(a[4])!, h = Double(a[5])!, secs = Double(a[6])!
    let app = NSApplication.shared
    app.setActivationPolicy(.accessory)
    let H = NSScreen.screens[0].frame.height
    let win = NSWindow(contentRect: NSRect(x: x, y: H - y - h, width: w, height: h), styleMask: [.borderless], backing: .buffered, defer: false)
    win.level = .floating; win.hasShadow = false; win.ignoresMouseEvents = true; win.animationBehavior = .none
    win.collectionBehavior = [.canJoinAllSpaces, .stationary]
    win.contentView = Card(frame: NSRect(x: 0, y: 0, width: w, height: h))
    win.orderFrontRegardless()
    DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { print("ready") }
    DispatchQueue.main.asyncAfter(deadline: .now() + secs) { exit(0) }
    app.run()
}

final class Grab: NSObject, SCStreamOutput, @unchecked Sendable {
    let lock = NSLock()
    var latest: CVPixelBuffer?
    var out = "", crop: [Int] = []
    // A still screen sends one frame and then nothing: keep the newest, write it after a moment.
    func stream(_ s: SCStream, didOutputSampleBuffer sb: CMSampleBuffer, of type: SCStreamOutputType) {
        guard type == .screen, let att = CMSampleBufferGetSampleAttachmentsArray(sb, createIfNecessary: false) as? [[SCStreamFrameInfo: Any]],
              let st = att.first?[.status] as? Int, st == SCFrameStatus.complete.rawValue, let pb = CMSampleBufferGetImageBuffer(sb) else { return }
        lock.lock(); latest = pb; lock.unlock()
    }
    func write() -> Bool {
        lock.lock(); let pb = latest; lock.unlock()
        guard let pb else { return false }
        CVPixelBufferLockBaseAddress(pb, .readOnly)
        let W = CVPixelBufferGetWidth(pb), H = CVPixelBufferGetHeight(pb), rb = CVPixelBufferGetBytesPerRow(pb)
        let base = CVPixelBufferGetBaseAddress(pb)!.assumingMemoryBound(to: UInt8.self)
        let (x0, y0, w, h) = crop.count == 4 ? (crop[0], crop[1], crop[2], crop[3]) : (0, 0, W, H)
        var d = Data("P6 \(w) \(h) 255\n".utf8)
        var row = [UInt8](repeating: 0, count: w * 3)
        for y in y0..<(y0 + h) {
            let p = base + y * rb
            for x in 0..<w { let o = (x0 + x) * 4; row[x * 3] = p[o + 2]; row[x * 3 + 1] = p[o + 1]; row[x * 3 + 2] = p[o] }
            d.append(contentsOf: row)
        }
        CVPixelBufferUnlockBaseAddress(pb, .readOnly)
        try? d.write(to: URL(fileURLWithPath: out))
        print("grabbed \(W)x\(H)")
        return true
    }
}

func grab() {
    let g = Grab()
    g.out = a[2]
    if a.count >= 7 { g.crop = a[3...6].map { Int($0)! } }
    Task {
        do {
            let content = try await SCShareableContent.excludingDesktopWindows(false, onScreenWindowsOnly: false)
            let disp = content.displays.first { $0.displayID == CGMainDisplayID() }!
            let me = content.applications.filter { $0.processID == getpid() }
            let c = SCStreamConfiguration()
            let px = Int(NSScreen.screens[0].backingScaleFactor)
            c.width = disp.width * px; c.height = disp.height * px
            c.showsCursor = false
            c.pixelFormat = kCVPixelFormatType_32BGRA
            c.minimumFrameInterval = CMTime(value: 1, timescale: 120)
            let s = SCStream(filter: SCContentFilter(display: disp, excludingApplications: me, exceptingWindows: []), configuration: c, delegate: nil)
            try s.addStreamOutput(g, type: .screen, sampleHandlerQueue: DispatchQueue(label: "grab"))
            try await s.startCapture()
            try await Task.sleep(nanoseconds: 400_000_000)
            try await s.stopCapture()
            exit(g.write() ? 0 : 1)
        } catch { print("error: \(error)"); exit(1) }
    }
    dispatchMain()
}

func patch() {
    let x = Double(a[2])!, y = Double(a[3])!, w = Double(a[4])!, h = Double(a[5])!
    let app = NSApplication.shared
    app.setActivationPolicy(.accessory)
    let H = NSScreen.screens[0].frame.height
    let win = NSWindow(contentRect: NSRect(x: x, y: H - y - h, width: w, height: h), styleMask: [.borderless], backing: .buffered, defer: false)
    win.level = .floating; win.hasShadow = false; win.ignoresMouseEvents = true; win.animationBehavior = .none
    win.collectionBehavior = [.canJoinAllSpaces, .stationary]
    win.backgroundColor = NSColor(srgbRed: 0.5, green: 0.5, blue: 0.5, alpha: 1)
    win.orderFrontRegardless()
    Thread.detachNewThread {
        while let l = readLine() {
            let p = l.split(separator: " ")
            DispatchQueue.main.async {
                if p.first == "move", p.count == 3 { win.setFrameOrigin(NSPoint(x: Double(p[1])!, y: H - Double(p[2])! - h)) }
                else if p.count == 3 { let c = p.map { CGFloat(Double($0) ?? 0) / 255 }; win.backgroundColor = NSColor(srgbRed: c[0], green: c[1], blue: c[2], alpha: 1) }
                print("ok")
            }
        }
        exit(0)
    }
    DispatchQueue.main.asyncAfter(deadline: .now() + 0.3) { print("ready") }
    app.run()
}

if a.count > 6 && a[1] == "card" { card() }
else if a.count > 5 && a[1] == "patch" { patch() }
else if a.count > 2 && a[1] == "grab" { grab() }
else { print("usage: scalecard card <x> <y> <w> <h> <seconds> | patch <x> <y> <w> <h> | grab <out.ppm> [x y w h]"); exit(2) }
