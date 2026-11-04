import Foundation

// The Metal source of the Render engine's xBR kernels, compiled at runtime (Upscaler): the
// Command Line Tools ship no offline Metal compiler, and a runtime compile needs no build step.
enum UpscaleShaders {
    static let source = #"""
// Wind for Mac: the Render engine's xBR filter as Metal compute kernels (renderScaling 1, as on Windows).
// A faithful port of Wind's Windows pixel shader (src/render/upscale_shaders.h), matched against it on
// 2026-11-03 (research/wind-mac/2026-11-03-upscale-render-path.md): same constants, same taps, same maths.
// xBR-lv2 (c) 2011-2022 Hyllian, MIT licence. See THIRD-PARTY-NOTICES.md.
// The view is the visible source rect [uvMin, uvMax] (normalised); one thread per output pixel.
// xBR runs in two passes: k_xbr_pre decides each visible source texel's edges once, k_xbr_post blends per
// output pixel (bit-identical to the one-pass shader, a quarter of its cost).
#include <metal_stdlib>
using namespace metal;
struct Params { float2 uvMin; float2 uvMax; float2 srcSize; float2 dstSize; };

constexpr sampler smpP(coord::normalized, address::clamp_to_edge, filter::nearest);
struct Ctx {
    texture2d<float, access::sample> tex;
    float2 srcSize;
    float scale;
    float3 tap(float2 t) const { return tex.sample(smpP, (t + 0.5) / srcSize, level(0)).rgb; }
};
static inline float2 UV(constant Params& p, uint2 gid) {
    return mix(p.uvMin, p.uvMax, (float2(gid) + 0.5) / p.dstSize);
}

// ---------------- xBR-lv2 (corner C, AA blending), arbitrary zoom ----------------
#define XBR_EQ_THRESHOLD 0.32
#define XBR_LV2_COEFFICIENT 0.30
#define lv2_cf (XBR_LV2_COEFFICIENT + 2.0)
constant float4 Ao = float4( 1.0, -1.0, -1.0, 1.0 );
constant float4 Bo = float4( 1.0,  1.0, -1.0,-1.0 );
constant float4 Co = float4( 1.5,  0.5, -0.5, 0.5 );
constant float4 Ax = float4( 1.0, -1.0, -1.0, 1.0 );
constant float4 Bx = float4( 0.5,  2.0, -0.5,-2.0 );
constant float4 Cx = float4( 1.0,  1.0, -0.5, 0.0 );
constant float4 Ay = float4( 1.0, -1.0, -1.0, 1.0 );
constant float4 By = float4( 2.0,  0.5, -2.0,-0.5 );
constant float4 Cy = float4( 2.0,  0.0, -1.0, 0.5 );
constant float4 Ci = float4(0.25, 0.25, 0.25, 0.25);
constant float3 v2f = float3(65536, 256, 1);
constant float3 Yw = float3(0.2627, 0.6780, 0.0593);
struct M43 { float3 c[4]; };
static inline M43 mk(float3 a, float3 b, float3 c, float3 d) { M43 m; m.c[0] = a; m.c[1] = b; m.c[2] = c; m.c[3] = d; return m; }
static inline float4 enc(M43 m) { return float4(dot(v2f, m.c[0]), dot(v2f, m.c[1]), dot(v2f, m.c[2]), dot(v2f, m.c[3])); }
static inline float4 LTE(float4 A, float4 B) { return step(A, B); }
static inline float4 LT(float4 A, float4 B) { return float4(A < B); }
static inline float4 NOT(float4 A) { return 1.0 - A; }
static inline float4 diffv(float4 A, float4 B) { return float4(A != B); }
static inline float dist(float3 A, float3 B) { return dot(abs(A - B), Yw); }
static inline float4 dist4(M43 A, M43 B) { return float4(dist(A.c[0], B.c[0]), dist(A.c[1], B.c[1]), dist(A.c[2], B.c[2]), dist(A.c[3], B.c[3])); }
static inline float4 eq(M43 A, M43 B) { return step(dist4(A, B), float4(XBR_EQ_THRESHOLD)); }
static inline float4 neq(M43 A, M43 B) { return 1.0 - eq(A, B); }
static inline float4 wdist(M43 a, M43 b, M43 c, M43 d, M43 e, M43 f, M43 g, M43 h) {
    return dist4(a, b) + dist4(a, c) + dist4(d, e) + dist4(d, f) + 4.0 * dist4(g, h);
}
// ---------------- xBR two-pass: texel decisions once per visible source texel ----------------
// Pass 1 (one thread per source texel in the view + margin): the binary edge decisions
// edri, edr, edr_l, edr_u and px (4 corners each, 20 bits). Pass 2 (per output pixel): 5 taps,
// the fp-dependent blend weights and the final mixes. Same maths as UpscaleXbr.
static uint XbrMask(thread const Ctx& cx, float2 t) {
    float3 A1 = cx.tap(t + float2(-1,-2)), B1 = cx.tap(t + float2(0,-2)), C1 = cx.tap(t + float2(1,-2));
    float3 A  = cx.tap(t + float2(-1,-1)), B  = cx.tap(t + float2(0,-1)), C  = cx.tap(t + float2(1,-1));
    float3 D  = cx.tap(t + float2(-1, 0)), E  = cx.tap(t),                F  = cx.tap(t + float2(1, 0));
    float3 G  = cx.tap(t + float2(-1, 1)), H  = cx.tap(t + float2(0, 1)), I  = cx.tap(t + float2(1, 1));
    float3 G5 = cx.tap(t + float2(-1, 2)), H5 = cx.tap(t + float2(0, 2)), I5 = cx.tap(t + float2(1, 2));
    float3 A0 = cx.tap(t + float2(-2,-1)), D0 = cx.tap(t + float2(-2, 0)), G0 = cx.tap(t + float2(-2, 1));
    float3 C4 = cx.tap(t + float2( 2,-1)), F4 = cx.tap(t + float2( 2, 0)), I4 = cx.tap(t + float2( 2, 1));
    M43 b = mk(B, D, H, F), c = mk(C, A, G, I), d = mk(D, H, F, B), e = mk(E, E, E, E);
    M43 f = mk(F, B, D, H), g = mk(G, I, C, A), h = mk(H, F, B, D), i = mk(I, C, A, G);
    M43 i4 = mk(I4, C1, A0, G5), i5 = mk(I5, C4, A1, G0), h5 = mk(H5, F4, B1, D0), f4 = mk(F4, B1, D0, H5);
    float4 b_ = enc(b), c_ = enc(c), e_ = enc(e);
    float4 d_ = b_.yzwx, f_ = b_.wxyz, g_ = c_.zwxy, h_ = b_.zwxy;
    float4 irlv0 = diffv(e_, f_) * diffv(e_, h_);
    float4 irlv1 = saturate(irlv0 * (neq(f, b) * neq(f, c) + neq(h, d) * neq(h, g) +
                   eq(e, i) * (neq(f, f4) * neq(f, i4) + neq(h, h5) * neq(h, i5)) + eq(e, g) + eq(e, c)));
    float4 irlv2l = diffv(e_, g_) * diffv(d_, g_);
    float4 irlv2u = diffv(e_, c_) * diffv(b_, c_);
    float4 wd1 = wdist(e, c, g, i, h5, f4, h, f);
    float4 wd2 = wdist(h, d, i5, f, i4, b, e, i);
    float4 d_fg = dist4(f, g);
    float4 d_hc = dist4(h, c);
    float4 edri  = LTE(wd1, wd2) * irlv0;
    float4 edr   = LT(wd1, wd2) * irlv1 * NOT(edri.yzwx * edri.wxyz);
    float4 edr_l = LTE(lv2_cf * d_fg, d_hc) * irlv2l * edr * (NOT(edri.yzwx) * eq(e, c));
    float4 edr_u = LTE(lv2_cf * d_hc, d_fg) * irlv2u * edr * (NOT(edri.wxyz) * eq(e, g));
    float4 px = LTE(dist4(e, f), dist4(e, h));
    uint4 m = uint4(edri > 0.5) | (uint4(edr > 0.5) << 1) | (uint4(edr_l > 0.5) << 2) | (uint4(edr_u > 0.5) << 3) | (uint4(px > 0.5) << 4);
    return m.x | (m.y << 5) | (m.z << 10) | (m.w << 15);
}
struct PreParams { int2 origin; int2 size; float2 srcSize; };
kernel void k_xbr_pre(texture2d<float, access::sample> src [[texture(0)]], texture2d<uint, access::write> mask [[texture(1)]],
                      constant PreParams& q [[buffer(0)]], uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= uint(q.size.x) || gid.y >= uint(q.size.y)) return;
    int2 t = q.origin + int2(gid);
    if (any(t < 0) || any(t >= int2(q.srcSize))) return;
    Ctx cx{src, q.srcSize, 1.0f};
    mask.write(uint4(XbrMask(cx, float2(t))), uint2(t));
}
static inline float4 bits4(uint m, uint k) { return float4((m >> k) & 1u, (m >> (k + 5)) & 1u, (m >> (k + 10)) & 1u, (m >> (k + 15)) & 1u); }
kernel void k_xbr_post(texture2d<float, access::sample> src [[texture(0)]], texture2d<float, access::write> dst [[texture(1)]],
                       texture2d<uint, access::read> mask [[texture(2)]],
                       constant Params& p [[buffer(0)]], uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= uint(p.dstSize.x) || gid.y >= uint(p.dstSize.y)) return;
    Ctx cx{src, p.srcSize, 1.0f / max((p.uvMax.x - p.uvMin.x) * p.srcSize.x / p.dstSize.x, 1e-6f)};
    float2 sp = UV(p, gid) * p.srcSize;
    float2 t = floor(sp);
    float2 fp = sp - t;
    uint m = mask.read(uint2(clamp(t, float2(0), p.srcSize - 1))).r;
    float aa = 2.0 / cx.scale;
    float3 B = cx.tap(t + float2(0,-1)), D = cx.tap(t + float2(-1, 0)), E = cx.tap(t), F = cx.tap(t + float2(1, 0)), H = cx.tap(t + float2(0, 1));
    float4 fx   = Ao * fp.y + Bo * fp.x;
    float4 fx_l = Ax * fp.y + Bx * fp.x;
    float4 fx_u = Ay * fp.y + By * fp.x;
    float4 delta  = float4(aa);
    float4 deltaL = float4(0.5, 1.0, 0.5, 1.0) * aa;
    float4 deltaU = deltaL.yxwz;
    float4 fx45i = bits4(m, 0) * saturate(0.5 + (fx   - Co - Ci) / delta);
    float4 fx45  = bits4(m, 1) * saturate(0.5 + (fx   - Co)      / delta);
    float4 fx30  = bits4(m, 2) * saturate(0.5 + (fx_l - Cx)      / deltaL);
    float4 fx60  = bits4(m, 3) * saturate(0.5 + (fx_u - Cy)      / deltaU);
    float4 px = bits4(m, 4);
    float4 mx = max(max(fx30, fx60), max(fx45, fx45i));
    float3 res1 = mix(E, mix(H, F, px.x), mx.x);
    float3 res2 = mix(E, mix(B, D, px.z), mx.z);
    float3 res1a = mix(res1, res2, step(dist(E, res1), dist(E, res2)));
    res1 = mix(E, mix(F, B, px.y), mx.y);
    res2 = mix(E, mix(D, H, px.w), mx.w);
    float3 res1b = mix(res1, res2, step(dist(E, res1), dist(E, res2)));
    dst.write(float4(saturate(mix(res1a, res1b, step(dist(E, res1a), dist(E, res1b)))), 1), gid);
}
"""#
}
