// probecap <seconds> <outfile> [savePNGevery]
// The ZOOMED screen frame by frame (a ScreenCaptureKit stream with the cursor whose filter excludes only
// this windowless process is WindowServer's zoomed output, docs/architecture.md) for Wind's own screenshot
// selection (scripts/drawshot_truth.py). Test builds draw probes (WIND_CURSOR_PROBE): magenta on the
// crosshair, yellow 20 px right of and below the box's far corner, cyan on the box's anchor corner.
// Per frame the centroid of each colour, in screen points (capture 1920 px wide = 1 px per point).
// A thread samples the pointer (CGEvent location, desktop points) every 1 ms.
// Lines (times in ns of the mach clock; F uses the frame's displayTime):
//   F t mx my mn yx yy yn cx cy cn      (-1 and count 0 where a colour is not found)
//   I t                                 an idle refresh (nothing on screen changed)
//   P t px py
import Foundation
import CoreGraphics
import ScreenCaptureKit
import CoreMedia
import CoreVideo
import ImageIO
import UniformTypeIdentifiers

let a = CommandLine.arguments
let secs = Double(a[1])!, outf = a[2]
let saveEvery = a.count > 3 ? Int(a[3])! : 0
var tb = mach_timebase_info(); mach_timebase_info(&tb)
func ns(_ m: UInt64) -> UInt64 { m * UInt64(tb.numer) / UInt64(tb.denom) }

func savePNG(_ pb: CVPixelBuffer, _ path: String) {
    let w = CVPixelBufferGetWidth(pb), h = CVPixelBufferGetHeight(pb), bpr = CVPixelBufferGetBytesPerRow(pb)
    guard let ctx = CGContext(data: CVPixelBufferGetBaseAddress(pb), width: w, height: h, bitsPerComponent: 8, bytesPerRow: bpr,
                              space: CGColorSpaceCreateDeviceRGB(),
                              bitmapInfo: CGImageAlphaInfo.premultipliedFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue),
          let img = ctx.makeImage() else { return }
    let d = CGImageDestinationCreateWithURL(URL(fileURLWithPath: path) as CFURL, UTType.png.identifier as CFString, 1, nil)!
    CGImageDestinationAddImage(d, img, nil); CGImageDestinationFinalize(d)
}

struct Acc { var x = 0.0, y = 0.0, w = 0.0, n = 0 }

final class Cap: NSObject, SCStreamOutput, @unchecked Sendable {
    var lines: [String] = []
    var n = 0
    var roi: [(Int, Int)?] = [nil, nil, nil]
    let work = DispatchQueue(label: "work")

    func stream(_ s: SCStream, didOutputSampleBuffer sb: CMSampleBuffer, of type: SCStreamOutputType) {
        guard type == .screen,
              let att = CMSampleBufferGetSampleAttachmentsArray(sb, createIfNecessary: false) as? [[SCStreamFrameInfo: Any]],
              let st = att.first?[.status] as? Int else { return }
        let dt = ns((att.first?[.displayTime] as? UInt64) ?? 0)
        if st == SCFrameStatus.idle.rawValue { work.async { self.lines.append("I \(dt)") }; return }
        guard st == SCFrameStatus.complete.rawValue, let pb = CMSampleBufferGetImageBuffer(sb) else { return }
        work.async { self.analyse(dt, pb) }
    }

    // 0 magenta, 1 yellow, 2 cyan
    static func kind(_ r: Int, _ g: Int, _ b: Int) -> Int {
        if r > 150 && b > 150 && g < 90 && abs(r - b) < 70 { return 0 }
        if r > 150 && g > 150 && b < 90 && abs(r - g) < 70 { return 1 }
        if g > 150 && b > 150 && r < 90 && abs(g - b) < 70 { return 2 }
        return -1
    }

    func analyse(_ t: UInt64, _ pb: CVPixelBuffer) {
        CVPixelBufferLockBaseAddress(pb, .readOnly); defer { CVPixelBufferUnlockBaseAddress(pb, .readOnly) }
        let w = CVPixelBufferGetWidth(pb), h = CVPixelBufferGetHeight(pb), bpr = CVPixelBufferGetBytesPerRow(pb)
        let base = CVPixelBufferGetBaseAddress(pb)!.assumingMemoryBound(to: UInt8.self)
        func scan(_ x0: Int, _ x1: Int, _ y0: Int, _ y1: Int, _ want: Set<Int>) -> [Acc] {
            var acc = [Acc(), Acc(), Acc()]
            for y in y0..<y1 {
                let p = base + y * bpr
                for x in x0..<x1 {
                    let o = x * 4
                    let k = Cap.kind(Int(p[o + 2]), Int(p[o + 1]), Int(p[o]))
                    if k >= 0 && want.contains(k) { acc[k].x += Double(x); acc[k].y += Double(y); acc[k].w += 1; acc[k].n += 1 }
                }
            }
            return acc
        }
        var res = [Acc(), Acc(), Acc()]
        var missing = Set<Int>()
        for k in 0..<3 {
            if let (cx, cy) = roi[k] {
                let m = 160
                let r = scan(max(0, cx - m), min(w, cx + m), max(0, cy - m), min(h, cy + m), [k])
                if r[k].n > 0 { res[k] = r[k] } else { missing.insert(k) }
            } else { missing.insert(k) }
        }
        if !missing.isEmpty {
            let r = scan(0, w, 0, h, missing)
            for k in missing { res[k] = r[k] }
        }
        var f = "F \(t)"
        for k in 0..<3 {
            let r = res[k]
            if r.n > 0 { let x = r.x / r.w, y = r.y / r.w; roi[k] = (Int(x), Int(y)); f += String(format: " %.3f %.3f %d", x * 1920 / Double(w), y * 1920 / Double(w), r.n) }
            else { roi[k] = nil; f += " -1 -1 0" }
        }
        lines.append(f)
        let idx = n; n += 1
        if saveEvery > 0 && idx % saveEvery == 0 { savePNG(pb, "\(outf).\(idx).png") }
    }
}

let o = Cap()
var pts: [String] = []
let ptLock = NSLock()
var sampling = true
let sampler = Thread {
    while sampling {
        if let e = CGEvent(source: nil) { let p = e.location; let s = "P \(ns(mach_absolute_time())) \(String(format: "%.4f %.4f", p.x, p.y))"; ptLock.lock(); pts.append(s); ptLock.unlock() }
        usleep(1000)
    }
}
let sem = DispatchSemaphore(value: 0)
Task {
    do {
        let content = try await SCShareableContent.excludingDesktopWindows(false, onScreenWindowsOnly: false)
        let disp = content.displays.first { $0.displayID == CGMainDisplayID() }!
        let me = content.applications.filter { $0.processID == getpid() }
        let c = SCStreamConfiguration()
        c.width = 1920; c.height = 1920 * disp.height / disp.width
        c.minimumFrameInterval = CMTime(value: 1, timescale: 240)
        c.showsCursor = true
        c.queueDepth = 8
        c.pixelFormat = kCVPixelFormatType_32BGRA
        let s = SCStream(filter: SCContentFilter(display: disp, excludingApplications: me, exceptingWindows: []), configuration: c, delegate: nil)
        try s.addStreamOutput(o, type: .screen, sampleHandlerQueue: DispatchQueue(label: "cap"))
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
o.work.sync {}
ptLock.lock()
let all = (o.lines + pts).sorted { (Int($0.split(separator: " ")[1]) ?? 0) < (Int($1.split(separator: " ")[1]) ?? 0) }
ptLock.unlock()
try? all.joined(separator: "\n").write(toFile: outf, atomically: true, encoding: .utf8)
print("frames \(o.lines.count)")
