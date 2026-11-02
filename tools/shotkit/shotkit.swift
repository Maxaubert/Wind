// shotkit: helpers for scripts/e2e_screenshot.py (macOS screenshot mode and the colour sampler while zoomed).
//   shotkit window X Y W H R G B   a click-through solid sRGB window at a desktop rect (top-left points),
//                                  floating level, title "WINDTEST shot"; prints "up", runs until killed
//   shotkit key CODE [cmd] [shift] [ctrl] [opt]   one key press posted at the HID tap
//   shotkit session                "up" while an interactive screenshot or the colour sampler holds the
//                                  pointer (the same test Wind uses), else "down"
//   shotkit sampler                NSColorSampler from an accessory app; prints "shown", then "picked R G B"
//                                  (sRGB) or "picked none"
//   shotkit coords X Y W H         like window, but every desktop point has its own colour (its x and y in
//                                  the raw display values), so a screenshot tells where it was taken
//   shotkit pattern X Y W H        like coords, for scripts/screenshot_truth.py: raw R = 128 + (x & 127),
//                                  G = 128 + (y & 127), B = 48 everywhere: never dark (macOS's crosshair arms
//                                  stay findable), the band's translucent fill gives every pixel under it
//                                  the same B, and R, G tell the desktop point modulo 128 where nothing covers it
//   shotkit clip save|restore FILE  the general pasteboard's items to/from a file (tests copy shots and put
//                                  the owner's clipboard back); shotkit clip png: "W H" of its image, or "none"
//   SHOTKIT_SOLID=1 makes window/coords/pattern windows take clicks (a stray click lands on the test's own window)
//   shotkit png FILE               "W H MAGENTA X0 Y0 X1 Y1": pixel size, how many pixels are Wind's magenta
//                                  probe, and the desktop points of the first and last pixel if from coords
import AppKit
import ImageIO

let a = CommandLine.arguments
let solid = ProcessInfo.processInfo.environment["SHOTKIT_SOLID"]
let SL = dlopen("/System/Library/PrivateFrameworks/SkyLight.framework/SkyLight", RTLD_LAZY)

func sessionUp() -> Bool {
    typealias Look = @convention(c) (mach_port_t, UnsafePointer<CChar>, UnsafeMutablePointer<mach_port_t>) -> kern_return_t
    let rt = UnsafeMutableRawPointer(bitPattern: -2)            // RTLD_DEFAULT
    if let f = dlsym(rt, "bootstrap_look_up"), let bp = dlsym(rt, "bootstrap_port") {
        var port: mach_port_t = 0
        if unsafeBitCast(f, to: Look.self)(bp.load(as: mach_port_t.self), "com.apple.screencapture.interactive", &port) == KERN_SUCCESS {
            mach_port_deallocate(mach_task_self_, port); return true
        }
    }
    typealias Conn = @convention(c) () -> Int32
    typealias Cap = @convention(c) (Int32, UnsafeMutablePointer<Bool>) -> Int32
    guard let h = SL, let c = dlsym(h, "SLSMainConnectionID"), let e = dlsym(h, "SLSAreEventsCaptured") else { return false }
    var b = false
    _ = unsafeBitCast(e, to: Cap.self)(unsafeBitCast(c, to: Conn.self)(), &b)
    guard b, let list = CGWindowListCopyWindowInfo([.optionOnScreenOnly], kCGNullWindowID) as? [[String: Any]] else { return false }
    return list.contains { ["screencapture", "ColorSampler"].contains($0[kCGWindowOwnerName as String] as? String ?? "") }
}

switch a.count > 1 ? a[1] : "" {
case "window":
    let v = a[2...8].map { Double($0)! }
    let app = NSApplication.shared
    app.setActivationPolicy(.accessory)
    let primaryH = NSScreen.screens[0].frame.height
    let w = NSWindow(contentRect: NSRect(x: v[0], y: primaryH - v[1] - v[3], width: v[2], height: v[3]),
                     styleMask: .borderless, backing: .buffered, defer: false)
    w.level = .floating; w.ignoresMouseEvents = solid == nil; w.isOpaque = true; w.hasShadow = false
    w.title = "WINDTEST shot"
    w.backgroundColor = NSColor(srgbRed: v[4], green: v[5], blue: v[6], alpha: 1)
    w.orderFrontRegardless()
    print("up"); fflush(stdout)
    app.run()
case "coords", "pattern":
    let pattern = a[1] == "pattern"
    // Raw values in the display's own colour space (no colour matching): R = x & 255, G = y & 255,
    // B = (x >> 8) << 3 | (y >> 8)
    // (B stays under 64, far from the magenta the e2e counts as Wind's cursor). One colour per point, 2 x 2 pixels at Retina.
    let v = a[2...5].map { Int($0)! }
    let app = NSApplication.shared
    app.setActivationPolicy(.accessory)
    let scr = NSScreen.screens[0], primaryH = scr.frame.height
    let cs = scr.colorSpace?.cgColorSpace ?? CGColorSpaceCreateDeviceRGB()
    let ctx = CGContext(data: nil, width: v[2], height: v[3], bitsPerComponent: 8, bytesPerRow: v[2] * 4, space: cs,
                        bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue)!
    let p = ctx.data!.assumingMemoryBound(to: UInt8.self)
    for j in 0..<v[3] { for i in 0..<v[2] {
        let x = v[0] + i, y = v[1] + j, o = (j * v[2] + i) * 4      // row 0 is the top row in memory
        if pattern { p[o] = UInt8(128 + x & 127); p[o + 1] = UInt8(128 + y & 127); p[o + 2] = 48; continue }
        p[o] = UInt8(x & 255); p[o + 1] = UInt8(y & 255); p[o + 2] = UInt8(((x >> 8) & 7) << 3 | ((y >> 8) & 7))
    } }
    let w = NSWindow(contentRect: NSRect(x: Double(v[0]), y: primaryH - Double(v[1] + v[3]), width: Double(v[2]), height: Double(v[3])),
                     styleMask: .borderless, backing: .buffered, defer: false)
    w.level = .floating; w.ignoresMouseEvents = solid == nil; w.isOpaque = true; w.hasShadow = false
    w.title = pattern ? "WINDTEST pattern" : "WINDTEST coords"; w.colorSpace = scr.colorSpace
    let view = NSView(); view.wantsLayer = true
    view.layer!.contents = ctx.makeImage(); view.layer!.magnificationFilter = .nearest; view.layer!.contentsGravity = .resize
    w.contentView = view
    w.orderFrontRegardless()
    print("up"); fflush(stdout)
    app.run()
case "key":
    var fl: CGEventFlags = []
    if a.contains("cmd") { fl.insert(.maskCommand) }
    if a.contains("shift") { fl.insert(.maskShift) }
    if a.contains("ctrl") { fl.insert(.maskControl) }
    if a.contains("opt") { fl.insert(.maskAlternate) }
    let src = CGEventSource(stateID: .hidSystemState)
    let kc = CGKeyCode(Int(a[2])!)
    let d = CGEvent(keyboardEventSource: src, virtualKey: kc, keyDown: true)!; d.flags = fl; d.post(tap: .cghidEventTap)
    usleep(30_000)
    let u = CGEvent(keyboardEventSource: src, virtualKey: kc, keyDown: false)!; u.flags = fl; u.post(tap: .cghidEventTap)
case "clip":
    let pb = NSPasteboard.general
    switch a[2] {
    case "save":
        var items: [[String: String]] = []
        for it in pb.pasteboardItems ?? [] {
            var d: [String: String] = [:]
            for t in it.types { if let v = it.data(forType: t) { d[t.rawValue] = v.base64EncodedString() } }
            items.append(d)
        }
        try! JSONSerialization.data(withJSONObject: items).write(to: URL(fileURLWithPath: a[3]))
        print("saved \(items.count) changeCount \(pb.changeCount)")
    case "restore":
        let items = try! JSONSerialization.jsonObject(with: Data(contentsOf: URL(fileURLWithPath: a[3]))) as! [[String: String]]
        pb.clearContents()
        let objs = items.map { d -> NSPasteboardItem in
            let it = NSPasteboardItem()
            for (t, v) in d { it.setData(Data(base64Encoded: v)!, forType: NSPasteboard.PasteboardType(t)) }
            return it
        }
        if !objs.isEmpty { pb.writeObjects(objs) }
        print("restored \(objs.count)")
    default:
        if let d = pb.data(forType: .png) ?? pb.data(forType: .tiff), let r = NSBitmapImageRep(data: d) { print("\(r.pixelsWide) \(r.pixelsHigh) \(pb.changeCount)") }
        else { print("none \(pb.changeCount)") }
    }
case "session":
    print(sessionUp() ? "up" : "down")
case "sampler":
    let app = NSApplication.shared
    app.setActivationPolicy(.accessory)
    DispatchQueue.main.async {
        NSColorSampler().show { c in
            if let s = c?.usingColorSpace(.sRGB) {
                print(String(format: "picked %.4f %.4f %.4f", s.redComponent, s.greenComponent, s.blueComponent))
            } else { print("picked none") }
            fflush(stdout); exit(0)
        }
        print("shown"); fflush(stdout)
    }
    DispatchQueue.main.asyncAfter(deadline: .now() + 20) { print("picked timeout"); fflush(stdout); exit(0) }
    app.run()
case "png":
    guard let src = CGImageSourceCreateWithURL(URL(fileURLWithPath: a[2]) as CFURL, nil),
          let img = CGImageSourceCreateImageAtIndex(src, 0, nil) else { print("0 0 0"); exit(0) }
    let w = img.width, h = img.height
    let ctx = CGContext(data: nil, width: w, height: h, bitsPerComponent: 8, bytesPerRow: w * 4,
                        space: CGColorSpace(name: CGColorSpace.sRGB)!, bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
    ctx.draw(img, in: CGRect(x: 0, y: 0, width: w, height: h))
    let p = ctx.data!.assumingMemoryBound(to: UInt8.self)
    var m = 0
    for i in 0..<(w * h) {
        let r = Int(p[i * 4]), g = Int(p[i * 4 + 1]), b = Int(p[i * 4 + 2])
        if r > 200 && b > 200 && g < 80 { m += 1 }           // the magenta block test builds draw for Wind's cursor
    }
    // The corners in the image's own colour space (raw values, as coords drew them).
    var raw = [UInt8](repeating: 0, count: w * h * 4)
    if let cs = img.colorSpace, let rc = CGContext(data: &raw, width: w, height: h, bitsPerComponent: 8, bytesPerRow: w * 4, space: cs,
                                                    bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue) {
        rc.draw(img, in: CGRect(x: 0, y: 0, width: w, height: h))
    }
    func at(_ i: Int) -> String {                                  // i: pixel index, row 0 at the top
        let r = Int(raw[i * 4]), g = Int(raw[i * 4 + 1]), b = Int(raw[i * 4 + 2])
        return "\((b >> 3 & 7) << 8 | r) \((b & 7) << 8 | g)"
    }
    print("\(w) \(h) \(m) \(at(0)) \(at(w * h - 1))")
default:
    fputs("usage: shotkit window|key|session|sampler|png ...\n", stderr); exit(2)
}
