// viewcap <seconds> <outfile> [width]
// Ground truth for the VIEW on the zoomed screen (same capture as tools/cursorcap: a stream with the
// cursor, excluding only this windowless process, is WindowServer's zoomed output). Per frame it
// writes the display time and a column profile (mean brightness of each pixel column, rows 10%-90%)
// and a row profile; scripts/edge_truth.py finds the content shift between frames from them.
// Output: binary, per frame: UInt64 displayTime(ns), Int32 w, Int32 h, Float32[w] cols, Float32[h] rows.
import Foundation
import CoreGraphics
import ScreenCaptureKit
import CoreMedia
import CoreVideo

let a = CommandLine.arguments
let secs = Double(a[1])!, outf = a[2]
let capW = a.count > 3 ? Int(a[3])! : 960
var tb = mach_timebase_info(); mach_timebase_info(&tb)

final class Out: NSObject, SCStreamOutput, @unchecked Sendable {
    var data = Data()
    var frames = 0
    let q = DispatchQueue(label: "work")
    func stream(_ s: SCStream, didOutputSampleBuffer sb: CMSampleBuffer, of type: SCStreamOutputType) {
        guard type == .screen,
              let att = CMSampleBufferGetSampleAttachmentsArray(sb, createIfNecessary: false) as? [[SCStreamFrameInfo: Any]],
              let st = att.first?[.status] as? Int, st == SCFrameStatus.complete.rawValue,
              let pb = CMSampleBufferGetImageBuffer(sb) else { return }
        let dt = (att.first?[.displayTime] as? UInt64) ?? 0
        let ns = dt * UInt64(tb.numer) / UInt64(tb.denom)
        CVPixelBufferLockBaseAddress(pb, .readOnly); defer { CVPixelBufferUnlockBaseAddress(pb, .readOnly) }
        let w = CVPixelBufferGetWidth(pb), h = CVPixelBufferGetHeight(pb), r = CVPixelBufferGetBytesPerRow(pb)
        let b = CVPixelBufferGetBaseAddress(pb)!.assumingMemoryBound(to: UInt8.self)
        var cols = [Float](repeating: 0, count: w), rows = [Float](repeating: 0, count: h)
        for y in 0..<h {
            let p = b + y * r
            var rs: Float = 0
            for x in 0..<w {
                let o = x * 4
                let v = Float(p[o]) + Float(p[o + 1]) + Float(p[o + 2])
                rs += v
                if y >= h / 10 && y < h * 9 / 10 { cols[x] += v }
            }
            rows[y] = rs / Float(w)
        }
        var ns2 = ns; var w32 = Int32(w), h32 = Int32(h)
        data.append(Data(bytes: &ns2, count: 8)); data.append(Data(bytes: &w32, count: 4)); data.append(Data(bytes: &h32, count: 4))
        cols.withUnsafeBufferPointer { data.append(Data(buffer: $0)) }
        rows.withUnsafeBufferPointer { data.append(Data(buffer: $0)) }
        frames += 1
    }
}

let o = Out()
let sem = DispatchSemaphore(value: 0)
Task {
    do {
        let content = try await SCShareableContent.excludingDesktopWindows(false, onScreenWindowsOnly: false)
        let disp = content.displays.first { $0.displayID == CGMainDisplayID() }!
        let me = content.applications.filter { $0.processID == getpid() }
        let c = SCStreamConfiguration()
        c.width = capW; c.height = capW * disp.height / disp.width
        c.minimumFrameInterval = CMTime(value: 1, timescale: 240)
        c.showsCursor = true
        c.queueDepth = 8
        c.pixelFormat = kCVPixelFormatType_32BGRA
        let s = SCStream(filter: SCContentFilter(display: disp, excludingApplications: me, exceptingWindows: []), configuration: c, delegate: nil)
        try s.addStreamOutput(o, type: .screen, sampleHandlerQueue: o.q)
        try await s.startCapture()
        print("capturing"); fflush(stdout)
        try await Task.sleep(nanoseconds: UInt64(secs * 1e9))
        try await s.stopCapture()
    } catch { print("error: \(error)") }
    sem.signal()
}
sem.wait()
o.q.sync {}
try? o.data.write(to: URL(fileURLWithPath: outf))
print("frames \(o.frames)")
