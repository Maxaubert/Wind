// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
// shotcap <seconds> <outfile> <level> <x0> <y0> <x1> <y1> [savePNGevery]
// Ground truth for macOS screenshot mode while zoomed (scripts/screenshot_truth.py): the ZOOMED screen
// frame by frame (a ScreenCaptureKit stream with the cursor whose filter excludes only this windowless
// process is WindowServer's zoomed output, docs/architecture.md), over a `shotkit pattern` window:
// raw R = 128 + (x & 127), G = 128 + (y & 127), B = 48 for desktop point (x, y). Analysed per frame in
// the screen rect x0 y0 x1 y1 (capture px, 1 px per screen point):
//   cursor  macOS's crosshair (screencapture's cursor, coordinate label baked in): its thin dark arms.
//           Per column the longest vertical run of pixels darker than both neighbours `d` px away, per
//           row the same horizontally; the hotspot is the middle of the columns / rows within 80% of
//           the longest run (an arm is `level` px thick). Runs shorter than 8 * level are ignored
//           (the label's digits are about 7 points tall; the ring at the centre splits each arm in two halves of
//           about 11 points).
//   band    screencapture's selection band: a translucent fill, so over the pattern (B = 48 everywhere)
//           every fill pixel has the same B. That B is the most common one in 58...160; the band's far
//           corner is the last column / row holding at least half the fullest one's fill pixels.
//   view    the zoomed view's top-left desktop point modulo 128, from pattern pixels in the bottom
//           right corner (never under a band dragged down-right from the centre); whole points.
// The pointer (CGEvent location, desktop points) and SLSGetZoomParameters are sampled every 1 ms on a
// thread of their own.
// Lines (times in ns of the mach clock, the displayTime of the frame):
//   F t hx hy hrunV hrunH fill bx by bn ox oy      (-1 where not found)
//   I t                                           an idle frame: nothing on screen changed that refresh
//   P t px py cx cy scale                         pointer and zoom readback (centre, level)
import Foundation
import CoreGraphics
import ScreenCaptureKit
import CoreMedia
import CoreVideo
import ImageIO
import UniformTypeIdentifiers

let a = CommandLine.arguments
let secs = Double(a[1])!, outf = a[2], level = Double(a[3])!
let rect = (Int(a[4])!, Int(a[5])!, Int(a[6])!, Int(a[7])!)
let saveEvery = a.count > 8 ? Int(a[8])! : 0
// SHOTCAP_NOANALYSE=1: frame times only (checks that the analysis does not cost refreshes).
let noAnalyse = ProcessInfo.processInfo.environment["SHOTCAP_NOANALYSE"] != nil
var tb = mach_timebase_info(); mach_timebase_info(&tb)
func ns(_ m: UInt64) -> UInt64 { m * UInt64(tb.numer) / UInt64(tb.denom) }

let SL = dlopen("/System/Library/PrivateFrameworks/SkyLight.framework/SkyLight", RTLD_LAZY)
typealias ConnFn = @convention(c) () -> Int32
typealias GetZoomFn = @convention(c) (Int32, UnsafeMutablePointer<CGPoint>, UnsafeMutablePointer<Double>, UnsafeMutablePointer<Bool>) -> Int32
let cid = unsafeBitCast(dlsym(SL, "SLSMainConnectionID")!, to: ConnFn.self)()
let getZoom = unsafeBitCast(dlsym(SL, "SLSGetZoomParameters")!, to: GetZoomFn.self)

func savePNG(_ pb: CVPixelBuffer, _ path: String) {
    let w = CVPixelBufferGetWidth(pb), h = CVPixelBufferGetHeight(pb), bpr = CVPixelBufferGetBytesPerRow(pb)
    guard let ctx = CGContext(data: CVPixelBufferGetBaseAddress(pb), width: w, height: h, bitsPerComponent: 8, bytesPerRow: bpr,
                              space: CGColorSpaceCreateDeviceRGB(),
                              bitmapInfo: CGImageAlphaInfo.premultipliedFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue),
          let img = ctx.makeImage() else { return }
    let d = CGImageDestinationCreateWithURL(URL(fileURLWithPath: path) as CFURL, UTType.png.identifier as CFString, 1, nil)!
    CGImageDestinationAddImage(d, img, nil); CGImageDestinationFinalize(d)
}

final class Cap: NSObject, SCStreamOutput, @unchecked Sendable {
    var lines: [String] = []
    var n = 0
    var last: (Int, Int)?
    let work = DispatchQueue(label: "work")
    let lock = NSLock()

    func stream(_ s: SCStream, didOutputSampleBuffer sb: CMSampleBuffer, of type: SCStreamOutputType) {
        guard type == .screen,
              let att = CMSampleBufferGetSampleAttachmentsArray(sb, createIfNecessary: false) as? [[SCStreamFrameInfo: Any]],
              let st = att.first?[.status] as? Int else { return }
        let dt = (att.first?[.displayTime] as? UInt64) ?? 0
        if noAnalyse { if st == SCFrameStatus.complete.rawValue || st == SCFrameStatus.idle.rawValue {
            work.async { self.lines.append((st == SCFrameStatus.idle.rawValue ? "I " : "F ") + "\(ns(dt))") } }; return }
        if st == SCFrameStatus.idle.rawValue { work.async { self.lines.append("I \(ns(dt))") }; return }
        guard st == SCFrameStatus.complete.rawValue, let pb = CMSampleBufferGetImageBuffer(sb) else { return }
        work.async { self.analyse(ns(dt), pb) }
    }

    var busy = 0.0, busyMax = 0.0
    func analyse(_ t: UInt64, _ pb: CVPixelBuffer) {
        let t0 = mach_absolute_time()
        defer { let d = Double(ns(mach_absolute_time() - t0)) / 1e6; busy += d; busyMax = max(busyMax, d) }
        CVPixelBufferLockBaseAddress(pb, .readOnly); defer { CVPixelBufferUnlockBaseAddress(pb, .readOnly) }
        let w = CVPixelBufferGetWidth(pb), h = CVPixelBufferGetHeight(pb), rb = CVPixelBufferGetBytesPerRow(pb)
        let base = CVPixelBufferGetBaseAddress(pb)!.assumingMemoryBound(to: UInt8.self)
        let x0 = max(0, rect.0), y0 = max(0, rect.1), x1 = min(w, rect.2), y1 = min(h, rect.3)
        @inline(__always) func px(_ x: Int, _ y: Int) -> UnsafeMutablePointer<UInt8> { base + y * rb + x * 4 }   // B G R A
        @inline(__always) func lum(_ x: Int, _ y: Int) -> Int { let p = px(x, y); return (Int(p[0]) + 2 * Int(p[1]) + Int(p[2])) >> 2 }

        // Band: the fill's B.
        var hist = [Int](repeating: 0, count: 256)
        for y in stride(from: y0, to: y1, by: 1) { let r = base + y * rb; for x in x0..<x1 { hist[Int(r[x * 4])] += 1 } }
        var fill = -1, best = 300
        for b in 58...160 where hist[b] > best { best = hist[b]; fill = b }
        var bx = -1, by = -1, bn = 0
        if fill >= 0 {
            var col = [Int](repeating: 0, count: w), row = [Int](repeating: 0, count: h)
            for y in y0..<y1 {
                let r = base + y * rb
                for x in x0..<x1 where abs(Int(r[x * 4]) - fill) <= 1 { col[x] += 1; row[y] += 1; bn += 1 }
            }
            let cm = col.max()!, rm = row.max()!
            if let i = col.lastIndex(where: { $0 * 2 >= cm }) { bx = i + 1 }
            if let i = row.lastIndex(where: { $0 * 2 >= rm }) { by = i + 1 }
        }

        // Cursor: dark thin arms.
        let L = max(1, Int(level.rounded()))
        let d = L + 3, T = 22, minRun = 8 * L
        let m = 60 * L + 40
        var cx0 = x0 + d, cx1 = x1 - d, cy0 = y0 + d, cy1 = y1 - d
        if let (lx, ly) = last { cx0 = max(cx0, lx - m); cx1 = min(cx1, lx + m); cy0 = max(cy0, ly - m); cy1 = min(cy1, ly + m) }
        var hx = -1.0, hy = -1.0, runV = 0, runH = 0
        if cx1 > cx0 && cy1 > cy0 {
            var vRun = [Int](repeating: 0, count: w), hRun = [Int](repeating: 0, count: h)
            var cur = [Int](repeating: 0, count: w)
            for y in cy0..<cy1 {
                var hr = 0, hbest = 0
                for x in cx0..<cx1 {
                    let c = lum(x, y)
                    let v = c + T < lum(x - d, y) && c + T < lum(x + d, y)
                    if v { cur[x] += 1; if cur[x] > vRun[x] { vRun[x] = cur[x] } } else { cur[x] = 0 }
                    let hh = c + T < lum(x, y - d) && c + T < lum(x, y + d)
                    if hh { hr += 1; if hr > hbest { hbest = hr } } else { hr = 0 }
                }
                hRun[y] = hbest
            }
            runV = vRun.max()!; runH = hRun.max()!
            if runV >= minRun && runH >= minRun {
                var sx = 0.0, nx = 0.0, sy = 0.0, ny = 0.0
                for x in cx0..<cx1 where Double(vRun[x]) >= 0.8 * Double(runV) { sx += Double(x); nx += 1 }
                for y in cy0..<cy1 where Double(hRun[y]) >= 0.8 * Double(runH) { sy += Double(y); ny += 1 }
                hx = sx / nx + 0.5; hy = sy / ny + 0.5
            }
        }
        last = hx >= 0 ? (Int(hx), Int(hy)) : nil

        // View: the top-left desktop point modulo 128, voted from the bottom right corner.
        var votes: [Int: Int] = [:]
        for j in 0..<6 { for i in 0..<6 {
            let sx = min(x1, w) - 20 - i * 37, sy = min(y1, h) - 20 - j * 23
            guard sx > 0, sy > 0 else { continue }
            let p = px(sx, sy)
            guard abs(Int(p[0]) - 48) <= 1, p[1] >= 128, p[2] >= 128 else { continue }
            let ox = ((Int(p[2]) - 128) - Int((Double(sx) / level).rounded(.down))) & 127
            let oy = ((Int(p[1]) - 128) - Int((Double(sy) / level).rounded(.down))) & 127
            votes[ox * 1000 + oy, default: 0] += 1
        } }
        let v = votes.max { $0.value < $1.value }
        let ox = v.map { $0.key / 1000 } ?? -1, oy = v.map { $0.key % 1000 } ?? -1

        lines.append(String(format: "F %llu %.2f %.2f %d %d %d %d %d %d %d %d", t, hx, hy, runV, runH, fill, bx, by, bn, ox, oy))
        let idx = n; n += 1
        if saveEvery > 0 && idx % saveEvery == 0 { savePNG(pb, "\(outf).\(idx).png") }
    }
}

// Pointer and zoom readback every 1 ms.
var samples: [String] = []
var sampling = true
let sampler = Thread {
    samples.reserveCapacity(20000)
    while sampling {
        let t = ns(mach_absolute_time())
        let p = CGEvent(source: nil)?.location ?? .zero
        var c = CGPoint.zero, s = 0.0, sm = false
        _ = getZoom(cid, &c, &s, &sm)
        samples.append(String(format: "P %llu %.4f %.4f %.3f %.3f %.4f", t, p.x, p.y, c.x, c.y, s))
        usleep(1000)
    }
}

let o = Cap()
let sem = DispatchSemaphore(value: 0)
Task {
    do {
        let content = try await SCShareableContent.excludingDesktopWindows(false, onScreenWindowsOnly: false)
        let disp = content.displays.first { $0.displayID == CGMainDisplayID() }!
        let me = content.applications.filter { $0.processID == getpid() }
        let c = SCStreamConfiguration()
        c.width = disp.width; c.height = disp.height
        c.minimumFrameInterval = CMTime(value: 1, timescale: 240)
        c.showsCursor = true
        c.queueDepth = 8
        c.pixelFormat = kCVPixelFormatType_32BGRA
        let s = SCStream(filter: SCContentFilter(display: disp, excludingApplications: me, exceptingWindows: []), configuration: c, delegate: nil)
        try s.addStreamOutput(o, type: .screen, sampleHandlerQueue: DispatchQueue(label: "a"))
        try await s.startCapture()
        sampler.start()
        print("capturing"); fflush(stdout)
        try await Task.sleep(nanoseconds: UInt64(secs * 1e9))
        try await s.stopCapture()
    } catch { print("error: \(error)") }
    sem.signal()
}
sem.wait()
sampling = false
usleep(5000)
o.work.sync {}
try? (o.lines + samples).joined(separator: "\n").write(toFile: outf, atomically: true, encoding: .utf8)
print(String(format: "frames %d analyse mean %.2f ms max %.2f ms", o.n, o.busy / Double(max(1, o.n)), o.busyMax))
