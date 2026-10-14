#pragma once
// Render-engine scaling filters (issue #456, chosen in the upscaler A/B of 2026-10-12, see
// research/wind/2026-10-12-upscaler-findings.md): xBR and NVIDIA Image Scaling (upscale_nis.h).
// Nearest keeps the original kMagHLSL pass. Each filter is one pixel shader built at runtime from
// kUpPrelude + kUpViewVS + its body + kUpMain (each literal stays under MSVC's 16 KB limit). The
// filters read texels through Tap(), which tonemaps first, so they run in display (sRGB) space and
// look the same on SDR and HDR.
//
// xBR-lv2 (c) 2011-2022 Hyllian, MIT licence, from the libretro slang shader (standalone, corner C,
// AA blending), adapted to HLSL and to arbitrary zoom levels.
namespace wind {

// renderScaling values (Config::renderScaling).
enum RenderScaling : int { kScaleNearest = 0, kScaleXbr = 1, kScaleNis = 2 };

inline constexpr const char* kUpPrelude = R"(
cbuffer CB : register(b0) {
    float2 uvMin; float2 uvMax;
    float brightness; float hdrMode; float scRgbScale; float sharpness;
    float texelW; float texelH; float colorOn; float pad;
    float4 cm0; float4 cm1; float4 cm2; float4 cm3; float4 cmOff;
};
struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
Texture2D tex : register(t0);
SamplerState smpL : register(s0);   // linear, clamp
SamplerState smpP : register(s1);   // point, clamp
float3 LinearToSrgb(float3 l) {
    l = saturate(l);
    return (l <= 0.0031308) ? (l * 12.92) : (1.055 * pow(l, 1.0 / 2.4) - 0.055);
}
float3 ToDisplay(float3 c) {
    if (hdrMode > 0.5) c = LinearToSrgb(max(c, 0.0) * scRgbScale);
    return c;
}
float3 ApplyColor(float3 c) {
    if (colorOn > 0.5) c = saturate((c.r * cm0 + c.g * cm1 + c.b * cm2 + cm3 + cmOff).rgb);
    return c * brightness;
}
float2 SrcSize() { return float2(1.0 / texelW, 1.0 / texelH); }
float2 Texel() { return float2(texelW, texelH); }
// Output pixels per source texel (the zoom level): the visible source width maps to the full output.
float Scale() { return 1.0 / max(uvMax.x - uvMin.x, 1e-6); }
// One source texel by integer coordinates, in display space.
float3 Tap(float2 t) { return ToDisplay(tex.SampleLevel(smpP, (t + 0.5) * Texel(), 0).rgb); }
float3 Bilin(float2 uv) { return ToDisplay(tex.SampleLevel(smpL, uv, 0).rgb); }
float Luma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }
)";

// The vertex shader for the filters: maps the screen onto the visible source rect.
inline constexpr const char* kUpViewVS = R"(
VSOut VSMain(uint id : SV_VertexID) {
    float2 t = float2((id << 1) & 2, id & 2);
    VSOut o;
    o.pos = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv  = lerp(uvMin, uvMax, t);
    return o;
}
)";

inline constexpr const char* kUpMain = R"(
float4 PSMain(VSOut i) : SV_TARGET {
    float3 c = Upscale(i.uv);
    c = ApplyColor(saturate(c));
    return float4(saturate(c), 1.0);
}
)";

// Hyllian's xBR-lv2 (corner C, AA blending).
inline constexpr const char* kUpSrcXbr = R"(
#define XBR_EQ_THRESHOLD 0.32
#define XBR_LV2_COEFFICIENT 0.30
#define lv2_cf (XBR_LV2_COEFFICIENT + 2.0)
static const float4 Ao = float4( 1.0, -1.0, -1.0, 1.0 );
static const float4 Bo = float4( 1.0,  1.0, -1.0,-1.0 );
static const float4 Co = float4( 1.5,  0.5, -0.5, 0.5 );
static const float4 Ax = float4( 1.0, -1.0, -1.0, 1.0 );
static const float4 Bx = float4( 0.5,  2.0, -0.5,-2.0 );
static const float4 Cx = float4( 1.0,  1.0, -0.5, 0.0 );
static const float4 Ay = float4( 1.0, -1.0, -1.0, 1.0 );
static const float4 By = float4( 2.0,  0.5, -2.0,-0.5 );
static const float4 Cy = float4( 2.0,  0.0, -1.0, 0.5 );
static const float4 Ci = float4(0.25, 0.25, 0.25, 0.25);
static const float3 v2f = float3(65536, 256, 1);
static const float3 Yw = float3(0.2627, 0.6780, 0.0593);
struct M43 { float3 c[4]; };
M43 mk(float3 a, float3 b, float3 c, float3 d) { M43 m; m.c[0] = a; m.c[1] = b; m.c[2] = c; m.c[3] = d; return m; }
float4 enc(M43 m) { return float4(dot(v2f, m.c[0]), dot(v2f, m.c[1]), dot(v2f, m.c[2]), dot(v2f, m.c[3])); }
float4 LTE(float4 A, float4 B) { return step(A, B); }
float4 LT(float4 A, float4 B) { return float4(A < B); }
float4 NOT(float4 A) { return 1.0 - A; }
float4 diffv(float4 A, float4 B) { return float4(A != B); }
float dist(float3 A, float3 B) { return dot(abs(A - B), Yw); }
float4 dist4(M43 A, M43 B) { return float4(dist(A.c[0], B.c[0]), dist(A.c[1], B.c[1]), dist(A.c[2], B.c[2]), dist(A.c[3], B.c[3])); }
float4 eq(M43 A, M43 B) { return step(dist4(A, B), XBR_EQ_THRESHOLD); }
float4 neq(M43 A, M43 B) { return 1.0 - eq(A, B); }
float4 wdist(M43 a, M43 b, M43 c, M43 d, M43 e, M43 f, M43 g, M43 h) {
    return dist4(a, b) + dist4(a, c) + dist4(d, e) + dist4(d, f) + 4.0 * dist4(g, h);
}
float3 Upscale(float2 uv) {
    float2 sp = uv * SrcSize();
    float2 t = floor(sp);
    float2 fp = sp - t;
    float aa = 2.0 / Scale();
    float3 A1 = Tap(t + float2(-1,-2)), B1 = Tap(t + float2(0,-2)), C1 = Tap(t + float2(1,-2));
    float3 A  = Tap(t + float2(-1,-1)), B  = Tap(t + float2(0,-1)), C  = Tap(t + float2(1,-1));
    float3 D  = Tap(t + float2(-1, 0)), E  = Tap(t),                F  = Tap(t + float2(1, 0));
    float3 G  = Tap(t + float2(-1, 1)), H  = Tap(t + float2(0, 1)), I  = Tap(t + float2(1, 1));
    float3 G5 = Tap(t + float2(-1, 2)), H5 = Tap(t + float2(0, 2)), I5 = Tap(t + float2(1, 2));
    float3 A0 = Tap(t + float2(-2,-1)), D0 = Tap(t + float2(-2, 0)), G0 = Tap(t + float2(-2, 1));
    float3 C4 = Tap(t + float2( 2,-1)), F4 = Tap(t + float2( 2, 0)), I4 = Tap(t + float2( 2, 1));
    M43 b = mk(B, D, H, F), c = mk(C, A, G, I), d = mk(D, H, F, B), e = mk(E, E, E, E);
    M43 f = mk(F, B, D, H), g = mk(G, I, C, A), h = mk(H, F, B, D), i = mk(I, C, A, G);
    M43 i4 = mk(I4, C1, A0, G5), i5 = mk(I5, C4, A1, G0), h5 = mk(H5, F4, B1, D0), f4 = mk(F4, B1, D0, H5);
    float4 b_ = enc(b), c_ = enc(c), e_ = enc(e);
    float4 d_ = b_.yzwx, f_ = b_.wxyz, g_ = c_.zwxy, h_ = b_.zwxy;
    float4 fx   = Ao * fp.y + Bo * fp.x;
    float4 fx_l = Ax * fp.y + Bx * fp.x;
    float4 fx_u = Ay * fp.y + By * fp.x;
    float4 irlv0 = diffv(e_, f_) * diffv(e_, h_);
    float4 irlv1 = saturate(irlv0 * (neq(f, b) * neq(f, c) + neq(h, d) * neq(h, g) +
                   eq(e, i) * (neq(f, f4) * neq(f, i4) + neq(h, h5) * neq(h, i5)) + eq(e, g) + eq(e, c)));
    float4 irlv2l = diffv(e_, g_) * diffv(d_, g_);
    float4 irlv2u = diffv(e_, c_) * diffv(b_, c_);
    float4 delta  = aa;
    float4 deltaL = float4(0.5, 1.0, 0.5, 1.0) * aa;
    float4 deltaU = deltaL.yxwz;
    float4 fx45i = saturate(0.5 + (fx   - Co - Ci) / delta);
    float4 fx45  = saturate(0.5 + (fx   - Co)      / delta);
    float4 fx30  = saturate(0.5 + (fx_l - Cx)      / deltaL);
    float4 fx60  = saturate(0.5 + (fx_u - Cy)      / deltaU);
    float4 wd1 = wdist(e, c, g, i, h5, f4, h, f);
    float4 wd2 = wdist(h, d, i5, f, i4, b, e, i);
    float4 d_fg = dist4(f, g);
    float4 d_hc = dist4(h, c);
    float4 edri  = LTE(wd1, wd2) * irlv0;
    float4 edr   = LT(wd1, wd2) * irlv1 * NOT(edri.yzwx * edri.wxyz);
    float4 edr_l = LTE(lv2_cf * d_fg, d_hc) * irlv2l * edr * (NOT(edri.yzwx) * eq(e, c));
    float4 edr_u = LTE(lv2_cf * d_hc, d_fg) * irlv2u * edr * (NOT(edri.wxyz) * eq(e, g));
    fx45i = edri * fx45i; fx45 = edr * fx45; fx30 = edr_l * fx30; fx60 = edr_u * fx60;
    float4 px = LTE(dist4(e, f), dist4(e, h));
    float4 mx = max(max(fx30, fx60), max(fx45, fx45i));
    float3 res1 = lerp(E, lerp(H, F, px.x), mx.x);
    float3 res2 = lerp(E, lerp(B, D, px.z), mx.z);
    float3 res1a = lerp(res1, res2, step(dist(E, res1), dist(E, res2)));
    res1 = lerp(E, lerp(F, B, px.y), mx.y);
    res2 = lerp(E, lerp(D, H, px.w), mx.w);
    float3 res1b = lerp(res1, res2, step(dist(E, res1), dist(E, res2)));
    return lerp(res1a, res1b, step(dist(E, res1a), dist(E, res1b)));
}
)";

}  // namespace wind
