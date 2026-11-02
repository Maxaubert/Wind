import Foundation

// View geometry (ports of Wind's edge_pan.h and view_glide.h). All coordinates are in the
// display's own points, top-left origin. The view is described by its centre and level: it shows
// screen/level of the desktop around that centre.

public struct Pt: Equatable { public var x, y: Double; public init(_ x: Double, _ y: Double) { self.x = x; self.y = y } }
public struct Sz: Equatable { public var w, h: Double; public init(_ w: Double, _ h: Double) { self.w = w; self.h = h } }
public struct Rc: Equatable {
    public var l, t, r, b: Double
    public init(l: Double, t: Double, r: Double, b: Double) { self.l = l; self.t = t; self.r = r; self.b = b }
    public var width: Double { r - l }
    public var height: Double { b - t }
}

/// Keep the view on the screen: the centre stays at least half a view from every edge.
public func clampCenter(_ c: Pt, level: Double, screen: Sz) -> Pt {
    let lv = max(1.0, level)
    let vw = screen.w / lv, vh = screen.h / lv
    return Pt(min(max(c.x, vw / 2), screen.w - vw / 2), min(max(c.y, vh / 2), screen.h - vh / 2))
}

/// The cursor's visible body around its hotspot, so the edge band is measured to the body.
public struct CursorBody: Equatable {
    public var l = 0.0, t = 0.0, r = 0.0, b = 0.0
    public init(l: Double = 0, t: Double = 0, r: Double = 0, b: Double = 0) { self.l = l; self.t = t; self.r = r; self.b = b }
    /// The standard macOS arrow (hotspot at the tip): about 12 x 19 points down and to the right.
    public static let arrow = CursorBody(l: 1, t: 1, r: 12, b: 19)
}

/// Edge mode: the view moves only when the pointer leaves the comfortable band, and just far enough.
public func edgePanCenter(cur: Pt, ptr: Pt, level: Double, screen: Sz, marginPct: Double, body: CursorBody = CursorBody()) -> Pt {
    let lv = max(1.0, level)
    let vw = screen.w / lv, vh = screen.h / lv
    let mx = vw * marginPct / 100, my = vh * marginPct / 100
    func axis(_ c: Double, _ p: Double, _ v: Double, _ m: Double, _ bLo: Double, _ bHi: Double) -> Double {
        var lo = c - v / 2 + m + bLo, hi = c + v / 2 - m - bHi
        if lo > hi { lo = (lo + hi) / 2; hi = lo }
        if p < lo { return c - (lo - p) }
        if p > hi { return c + (p - hi) }
        return c
    }
    let c = Pt(axis(cur.x, ptr.x, vw, mx, body.l, body.r), axis(cur.y, ptr.y, vh, my, body.t, body.b))
    return clampCenter(c, level: lv, screen: screen)
}

/// Screenshot or colour-sampler mode (macOS's zoom and its own cursor): the view holds still while the
/// pointer is inside the band of the view shown now (`cur`), so the real cursor and the selection band move
/// over a steady picture. Once the pointer enters the margin on an axis, the target on that axis moves past
/// the pointer by `leadPct` of the view (the view glides there, leaving room ahead, so a long drag pans in
/// few glides); otherwise the target is kept. Returns the new target.
public func stillViewTarget(_ target: Pt, cur: Pt, ptr: Pt, level: Double, screen: Sz, marginPct: Double, leadPct: Double = 0) -> Pt {
    let lv = max(1.0, level)
    let vw = screen.w / lv, vh = screen.h / lv
    func axis(_ t: Double, _ c: Double, _ p: Double, _ v: Double) -> Double {
        let m = v * marginPct / 100, lead = v * leadPct / 100
        if p < c - v / 2 + m { return p - lead }
        if p > c + v / 2 - m { return p + lead }
        return t
    }
    return clampCenter(Pt(axis(target.x, cur.x, ptr.x, vw), axis(target.y, cur.y, ptr.y, vh)), level: lv, screen: screen)
}

/// Critically damped spring (SmoothDamp form, stable at any dt). Carries its velocity across
/// retargets, so a caret stepping per keystroke becomes one continuous glide. 95% in glideMs.
public func springToward(_ cur: Double, _ target: Double, vel: inout Double, dtMs: Double, glideMs: Double) -> Double {
    if glideMs <= 0 { vel = 0; return target }
    if dtMs <= 0 { return cur }
    let omega = 4.74 / (glideMs / 1000), dt = dtMs / 1000
    let x = omega * dt
    let decay = 1.0 / (1.0 + x + 0.48 * x * x + 0.235 * x * x * x)
    let change = cur - target
    let temp = (vel + omega * change) * dt
    vel = (vel - omega * temp) * decay
    return target + (change + temp) * decay
}

/// Where the view should go to show a caret/focus rect. align 0 = centred on it, 1 = only far
/// enough to bring it inside the margin band. nil for bogus rects.
public func trackTargetCenter(_ rc: Rc, cur: Pt, level: Double, screen: Sz, align: Int, marginPct: Double) -> Pt? {
    let lv = max(1.0, level)
    let w = rc.width, h = rc.height
    if w < 0 || h < 0 || (w == 0 && h == 0) { return nil }
    if rc.l == 0 && rc.t == 0 && rc.r == 0 && rc.b == 0 { return nil }
    if rc.r < 0 || rc.b < 0 || rc.l > screen.w || rc.t > screen.h { return nil }
    let vw = screen.w / lv, vh = screen.h / lv
    var c = cur
    if align == 0 {
        c = Pt((rc.l + rc.r) / 2, (rc.t + rc.b) / 2)
    } else {
        let mx = vw * marginPct / 100, my = vh * marginPct / 100
        func axis(_ c: Double, _ lo: Double, _ hi: Double, _ v: Double, _ m: Double) -> Double {
            let inLo = c - v / 2 + m, inHi = c + v / 2 - m
            if hi - lo > inHi - inLo { return lo - m + v / 2 }
            if lo < inLo { return c - (inLo - lo) }
            if hi > inHi { return c + (hi - inHi) }
            return c
        }
        c = Pt(axis(cur.x, rc.l, rc.r, vw, mx), axis(cur.y, rc.t, rc.b, vh, my))
    }
    return clampCenter(c, level: lv, screen: screen)
}

/// Keyboard panning speed in points per second of desktop, at a given level: up to 1.25 screens
/// (of the zoomed view) per second at panSpeed 1, a little slower at low zoom.
public func keyPanSpeed(level: Double, screenWidth: Double, panSpeed: Double) -> Double {
    let lv = max(1.0, level)
    let viewW = screenWidth / lv
    let lowZoomEase = min(1.0, 0.5 + 0.5 * (lv - 1.0) / 2.0)
    return viewW * 1.25 * panSpeed * lowZoomEase
}

/// One Euro filter (Casiez et al. 2012): a low-pass whose cutoff rises with speed, so slow jitter is
/// smoothed strongly and real motion follows with little lag. Used for the render engine's pan
/// smoothing (config panSmoothMs) to steady the view against hand jitter at high zoom.
public struct OneEuro {
    public var minCutoff: Double            // Hz at rest
    public var beta: Double                 // cutoff added per unit/s of speed
    public var dCutoff = 1.0                // Hz, for the speed estimate
    var x: Double?
    var dx = 0.0

    public init(minCutoff: Double, beta: Double) { self.minCutoff = minCutoff; self.beta = beta }

    public mutating func reset() { x = nil; dx = 0 }

    /// Filter a sample `v` taken `dt` s after the last; `scale` converts speed into the units beta uses.
    public mutating func filter(_ v: Double, dt: Double, scale: Double = 1) -> Double {
        guard let px = x, dt > 0 else { x = v; dx = 0; return v }
        func alpha(_ fc: Double) -> Double { let tau = 1 / (2 * Double.pi * fc); return 1 / (1 + tau / dt) }
        let rawDx = (v - px) / dt
        dx += alpha(dCutoff) * (rawDx - dx)
        let fc = minCutoff + beta * abs(dx) * scale
        let nx = px + alpha(fc) * (v - px)
        x = nx
        return nx
    }
}
