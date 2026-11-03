import Foundation
import IOSurface
import Metal
import QuartzCore
import WindCore

// The Render engine's own upscaler (renderScaling xBR, as on Windows): one Metal compute pass per
// changed frame from the captured IOSurface into a drawable of the overlay's CAMetalLayer, shown 1:1.
// Nearest needs none of this (the image layer's own filter).
// DrawZoom drives it: a pass is encoded on a tick and shown on the next one (one refresh of latency,
// never a wait inside the tick that encoded it), and only when the view or the captured frame changed.
//
// Measured in Wind (scripts/scaling_truth.py, M6, 4K at 144 Hz, 2026-11-03): GPU per pass while panning
// xBR 1.3-1.5 ms (median; the GPU runs at low clocks under this paced load); a still
// view runs no pass. The prototype: research/wind-mac/2026-11-03-upscale-render-path.md.

final class Upscaler: @unchecked Sendable {
    struct View: Equatable { var uvMin: SIMD2<Float>; var uvMax: SIMD2<Float> }
    private struct Params { var uvMin: SIMD2<Float>; var uvMax: SIMD2<Float>; var srcSize: SIMD2<Float>; var dstSize: SIMD2<Float> }
    private struct PreParams { var origin: SIMD2<Int32>; var size: SIMD2<Int32>; var srcSize: SIMD2<Float> }

    let device: MTLDevice
    private let queue: MTLCommandQueue
    private let xbrPre: MTLComputePipelineState, xbrPost: MTLComputePipelineState
    private var mask: MTLTexture?                       // xBR's per-texel edge decisions (r32Uint, source size)
    private var sources: [IOSurfaceID: MTLTexture] = [:]

    /// GPU time of finished passes (ms), for the log and the tests; written from Metal's completion thread.
    private let statsLock = NSLock()
    private var gpuMs: [Double] = []
    private var passes = 0

    /// Builds the kernels (a runtime compile, ~0.1-0.3 s: call it off the main thread). nil without
    /// Metal or when a kernel does not build; Render then uses Nearest instead (logged once).
    static func make() -> Upscaler? {
        guard let dev = MTLCreateSystemDefaultDevice(), let q = dev.makeCommandQueue() else {
            Log.error("render scaling: no Metal device; using Nearest"); return nil
        }
        do {
            let o = MTLCompileOptions()
            if #available(macOS 15.0, *) { o.mathMode = .fast } else { o.fastMathEnabled = true }
            let lib = try dev.makeLibrary(source: UpscaleShaders.source, options: o)
            func pso(_ n: String) throws -> MTLComputePipelineState {
                guard let f = lib.makeFunction(name: n) else { throw NSError(domain: "Wind", code: 3, userInfo: [NSLocalizedDescriptionKey: "no kernel \(n)"]) }
                return try dev.makeComputePipelineState(function: f)
            }
            return Upscaler(device: dev, queue: q, xbrPre: try pso("k_xbr_pre"), xbrPost: try pso("k_xbr_post"))
        } catch {
            Log.error("render scaling: the Metal kernels did not build (\(error.localizedDescription)); using Nearest")
            return nil
        }
    }

    private init(device: MTLDevice, queue: MTLCommandQueue, xbrPre: MTLComputePipelineState, xbrPost: MTLComputePipelineState) {
        self.device = device; self.queue = queue; self.xbrPre = xbrPre; self.xbrPost = xbrPost
    }

    /// The visible source rect (normalised, top-left origin) for a view centred on `center` (desktop
    /// points, top-left origin) at `scale`, on a screen of `size` points.
    static func view(center: CGPoint, scale: Double, size: CGSize) -> View {
        let vw = size.width / scale, vh = size.height / scale
        let x0 = (center.x - vw / 2) / size.width, y0 = (center.y - vh / 2) / size.height
        return View(uvMin: [Float(x0), Float(y0)], uvMax: [Float(x0 + vw / size.width), Float(y0 + vh / size.height)])
    }

    /// The colour space the captured frames carry (the display's), for the CAMetalLayer: without it Core
    /// Animation takes the pass's output for sRGB and converts it (desaturated, brownish; prototype 2026-11-03).
    static func colorSpace(of s: IOSurface) -> CGColorSpace? {
        guard let v = IOSurfaceCopyValue(unsafeBitCast(s, to: IOSurfaceRef.self), "IOSurfaceColorSpace" as CFString) else { return nil }
        if CFGetTypeID(v) == CFDataGetTypeID() { return CGColorSpace(iccData: v as! CFData) }
        if CFGetTypeID(v) == CFStringGetTypeID() { return CGColorSpace(name: v as! CFString) }
        return nil
    }

    /// Frees the mask and the cached capture textures (the capture stopped or the mode changed).
    func release() {
        mask = nil; sources = [:]
    }

    private func source(_ s: IOSurface) -> MTLTexture? {
        let id = IOSurfaceGetID(unsafeBitCast(s, to: IOSurfaceRef.self))
        if let t = sources[id], t.width == s.width, t.height == s.height { return t }
        if sources.count > 12 { sources = [:] }          // ScreenCaptureKit cycles a handful of surfaces
        let d = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .bgra8Unorm, width: s.width, height: s.height, mipmapped: false)
        d.usage = [.shaderRead]; d.storageMode = .shared
        let t = device.makeTexture(descriptor: d, iosurface: s, plane: 0)
        sources[id] = t
        return t
    }

    /// Encodes and commits one xBR pass over `s` for `view` into `dst` (a drawable's
    /// texture). Returns its command buffer (wait for it before presenting), or nil when it cannot run.
    /// `hold` (the capture's CVPixelBuffer) is kept until the GPU is done with it: the texture keeps only the
    /// IOSurface, and ScreenCaptureKit reuses a buffer for a new frame once nobody holds it.
    func encode(src s: IOSurface, view: View, into dst: MTLTexture, hold: AnyObject? = nil) -> MTLCommandBuffer? {
        guard let src = source(s), let cb = queue.makeCommandBuffer() else { return nil }
        let W = Float(src.width), H = Float(src.height)
        var p = Params(uvMin: view.uvMin, uvMax: view.uvMax, srcSize: [W, H], dstSize: [Float(dst.width), Float(dst.height)])
        let tg = MTLSize(width: 16, height: 16, depth: 1)
        if mask.map({ $0.width != src.width || $0.height != src.height }) ?? true {
            let md = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .r32Uint, width: src.width, height: src.height, mipmapped: false)
            md.usage = [.shaderWrite, .shaderRead]; md.storageMode = .private
            mask = device.makeTexture(descriptor: md)
        }
        guard let m = mask, let e1 = cb.makeComputeCommandEncoder() else { return nil }
        // Pass 1 over the visible source texels plus the 3-texel margin pass 2's taps reach.
        let a = Int((p.uvMin.x * W).rounded(.down)) - 3, b = Int((p.uvMin.y * H).rounded(.down)) - 3
        let c = Int((p.uvMax.x * W).rounded(.up)) + 3, d = Int((p.uvMax.y * H).rounded(.up)) + 3
        var pp = PreParams(origin: [Int32(a), Int32(b)], size: [Int32(c - a), Int32(d - b)], srcSize: [W, H])
        e1.setComputePipelineState(xbrPre); e1.setTexture(src, index: 0); e1.setTexture(m, index: 1)
        e1.setBytes(&pp, length: MemoryLayout<PreParams>.stride, index: 0)
        e1.dispatchThreads(MTLSize(width: c - a, height: d - b, depth: 1), threadsPerThreadgroup: tg)
        e1.endEncoding()
        guard let e2 = cb.makeComputeCommandEncoder() else { return nil }
        e2.setComputePipelineState(xbrPost); e2.setTexture(src, index: 0); e2.setTexture(dst, index: 1); e2.setTexture(m, index: 2)
        e2.setBytes(&p, length: MemoryLayout<Params>.stride, index: 0)
        e2.dispatchThreads(MTLSize(width: dst.width, height: dst.height, depth: 1), threadsPerThreadgroup: tg)
        e2.endEncoding()
        cb.addCompletedHandler { [weak self] b in
            withExtendedLifetime(hold) {}
            guard let self, b.status == .completed else { return }
            let ms = (b.gpuEndTime - b.gpuStartTime) * 1000
            self.statsLock.lock(); self.passes += 1; self.gpuMs.append(ms); if self.gpuMs.count > 4000 { self.gpuMs.removeFirst(2000) }; self.statsLock.unlock()
        }
        cb.commit()
        return cb
    }

    /// Passes finished and their GPU time (median and 95th percentile, ms) since the last reset.
    func stats(reset: Bool = false) -> (passes: Int, median: Double, p95: Double) {
        statsLock.lock(); defer { statsLock.unlock() }
        let s = gpuMs.sorted()
        let r = (passes, s.isEmpty ? 0 : s[s.count / 2], s.isEmpty ? 0 : s[min(s.count - 1, s.count * 95 / 100)])
        if reset { gpuMs = []; passes = 0 }
        return r
    }
}
