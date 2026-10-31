// cursorcap <seconds> <outfile> [savePNGevery] [width]
// Ground truth for the ZOOMED screen, frame by frame (what the owner sees). Measured 2026-10-31,
// macOS 27.2: a ScreenCaptureKit stream with showsCursor = true whose filter excludes only this
// windowless process is WindowServer's zoomed output, one frame per display refresh (144 Hz here).
// A filter that excludes any app with a window on screen, or showsCursor = false, gives the
// UNZOOMED desktop (the stream is then recomposited without the zoom), as does screencapture.
// Per frame: the centroid of the magenta block that test builds draw for Wind's cursor and
// Inspect's crosshair (WIND_CURSOR_PROBE), in screen points (capture `width` px wide, 1920 = 1 px
// per point). A second stream without the cursor is diffed against it (cx cy ccount), but since
// that stream is unzoomed the diff is NOT the real cursor: ignore those columns.
// Line: displayTime(ns) mx my mcount cx cy ccount
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
let capW = a.count > 4 ? Int(a[4])! : 1920
var tb = mach_timebase_info(); mach_timebase_info(&tb)

func savePNG(_ pb: CVPixelBuffer, _ path: String) {
    CVPixelBufferLockBaseAddress(pb, .readOnly); defer { CVPixelBufferUnlockBaseAddress(pb, .readOnly) }
    let w = CVPixelBufferGetWidth(pb), h = CVPixelBufferGetHeight(pb), bpr = CVPixelBufferGetBytesPerRow(pb)
    guard let ctx = CGContext(data: CVPixelBufferGetBaseAddress(pb), width: w, height: h, bitsPerComponent: 8, bytesPerRow: bpr,
                              space: CGColorSpaceCreateDeviceRGB(),
                              bitmapInfo: CGImageAlphaInfo.premultipliedFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue),
          let img = ctx.makeImage() else { return }
    let d = CGImageDestinationCreateWithURL(URL(fileURLWithPath: path) as CFURL, UTType.png.identifier as CFString, 1, nil)!
    CGImageDestinationAddImage(d, img, nil); CGImageDestinationFinalize(d)
}

struct Hit { var mx = 0.0, my = 0.0, mw = 0.0, mn = 0, dx = 0.0, dy = 0.0, dw = 0.0, dn = 0 }

final class Pair: NSObject, SCStreamOutput, @unchecked Sendable {
    let lock = NSLock()
    var a: [UInt64: CVPixelBuffer] = [:], b: [UInt64: CVPixelBuffer] = [:]
    var lines: [String] = []
    var n = 0
    var sA: SCStream?
    var countA = 0, countB = 0
    var roi: (Int, Int)?
    let work = DispatchQueue(label: "work")
    func stream(_ s: SCStream, didOutputSampleBuffer sb: CMSampleBuffer, of type: SCStreamOutputType) {
        guard type == .screen,
              let att = CMSampleBufferGetSampleAttachmentsArray(sb, createIfNecessary: false) as? [[SCStreamFrameInfo: Any]],
              let st = att.first?[.status] as? Int, st == SCFrameStatus.complete.rawValue,
              let pb = CMSampleBufferGetImageBuffer(sb) else { return }
        let dt = (att.first?[.displayTime] as? UInt64) ?? 0
        lock.lock()
        if s === sA { a[dt] = pb; countA += 1 } else { b[dt] = pb; countB += 1 }
        let pa = a[dt], pbB = b[dt]
        if pa != nil && pbB != nil { a[dt] = nil; b[dt] = nil }
        if a.count > 16 { for k in a.keys.sorted().prefix(a.count - 16) { a[k] = nil } }
        if b.count > 16 { for k in b.keys.sorted().prefix(b.count - 16) { b[k] = nil } }
        lock.unlock()
        if let pa, let pbB { work.async { self.diff(dt, pa, pbB) } }
    }
    func diff(_ dt: UInt64, _ A: CVPixelBuffer, _ B: CVPixelBuffer) {
        let ns = dt * UInt64(tb.numer) / UInt64(tb.denom)
        CVPixelBufferLockBaseAddress(A, .readOnly); CVPixelBufferLockBaseAddress(B, .readOnly)
        let w = CVPixelBufferGetWidth(A), h = CVPixelBufferGetHeight(A)
        let ra = CVPixelBufferGetBytesPerRow(A), rb = CVPixelBufferGetBytesPerRow(B)
        let ba = CVPixelBufferGetBaseAddress(A)!.assumingMemoryBound(to: UInt8.self)
        let bb = CVPixelBufferGetBaseAddress(B)!.assumingMemoryBound(to: UInt8.self)
        func scan(_ x0: Int, _ x1: Int, _ y0: Int, _ y1: Int) -> Hit {
            var r = Hit()
            for y in y0..<y1 {
                let pa = ba + y * ra, pb = bb + y * rb
                for x in x0..<x1 {
                    let o = x * 4
                    let b0 = Int(pa[o]), g0 = Int(pa[o + 1]), r0 = Int(pa[o + 2])
                    if r0 > 120 && b0 > 120 && g0 < 100 && abs(r0 - b0) < 60 {   // magenta probe
                        let wt = Double(min(r0, b0) - g0)
                        r.mx += wt * Double(x); r.my += wt * Double(y); r.mw += wt; r.mn += 1
                        continue
                    }
                    let d = abs(b0 - Int(pb[o])) + abs(g0 - Int(pb[o + 1])) + abs(r0 - Int(pb[o + 2]))
                    if d > 60 { let wt = Double(d); r.dx += wt * Double(x); r.dy += wt * Double(y); r.dw += wt; r.dn += 1 }
                }
            }
            return r
        }
        var r: Hit
        if let (cx, cy) = roi {
            let m = 120 * w / 1920
            r = scan(max(0, cx - m), min(w, cx + m), max(0, cy - m), min(h, cy + m))
            if r.mn == 0 && r.dn == 0 { r = scan(0, w, 0, h) }
        } else { r = scan(0, w, 0, h) }
        CVPixelBufferUnlockBaseAddress(A, .readOnly); CVPixelBufferUnlockBaseAddress(B, .readOnly)
        let k = Double(w) / 1920.0
        let mx = r.mw > 0 ? r.mx / r.mw / k : -1, my = r.mw > 0 ? r.my / r.mw / k : -1
        let cx = r.dw > 0 ? r.dx / r.dw / k : -1, cy = r.dw > 0 ? r.dy / r.dw / k : -1
        lines.append(String(format: "%llu %.4f %.4f %d %.4f %.4f %d", ns, mx, my, r.mn, cx, cy, r.dn))
        if r.mn > 0 { roi = (Int(mx * k), Int(my * k)) } else if r.dn > 0 { roi = (Int(cx * k), Int(cy * k)) }
        let idx = n; n += 1
        if saveEvery > 0 && idx % saveEvery == 0 { savePNG(A, "\(outf).\(idx).A.png"); savePNG(B, "\(outf).\(idx).B.png") }
    }
}

let o = Pair()
let sem = DispatchSemaphore(value: 0)
Task {
    do {
        let content = try await SCShareableContent.excludingDesktopWindows(false, onScreenWindowsOnly: false)
        let disp = content.displays.first { $0.displayID == CGMainDisplayID() }!
        let me = content.applications.filter { $0.processID == getpid() }
        func cfg(_ cursor: Bool) -> SCStreamConfiguration {
            let c = SCStreamConfiguration()
            c.width = capW; c.height = capW * disp.height / disp.width
            c.minimumFrameInterval = CMTime(value: 1, timescale: 240)
            c.showsCursor = cursor
            c.queueDepth = 8
            c.pixelFormat = kCVPixelFormatType_32BGRA
            return c
        }
        let f = SCContentFilter(display: disp, excludingApplications: me, exceptingWindows: [])
        let sA = SCStream(filter: f, configuration: cfg(true), delegate: nil)
        let sB = SCStream(filter: f, configuration: cfg(false), delegate: nil)
        o.sA = sA
        try sA.addStreamOutput(o, type: .screen, sampleHandlerQueue: DispatchQueue(label: "a"))
        try sB.addStreamOutput(o, type: .screen, sampleHandlerQueue: DispatchQueue(label: "b"))
        try await sA.startCapture(); try await sB.startCapture()
        print("capturing"); fflush(stdout)
        try await Task.sleep(nanoseconds: UInt64(secs * 1e9))
        try await sA.stopCapture(); try await sB.stopCapture()
    } catch { print("error: \(error)") }
    sem.signal()
}
sem.wait()
o.work.sync {}
try? o.lines.sorted().joined(separator: "\n").write(toFile: outf, atomically: true, encoding: .utf8)
print("frames A \(o.countA) B \(o.countB) paired \(o.lines.count)")
