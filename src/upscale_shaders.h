#pragma once
// PROTOTYPE (proto/upscalers, research/wind/2026-10-12-upscaler-prototype-spec.md): the render
// engine's selectable upscale filters. Filters 0 (nearest) and 1 (bilinear) keep the original
// kMagHLSL path untouched; every other filter is one pixel shader built by concatenating
// kUpPrelude + its own body + kUpMain at runtime (each literal stays under MSVC's 16 KB limit).
// The custom filters read texels through Tap(), which tonemaps first, so they run in display
// (sRGB) space like DWM's resampler, on SDR and HDR alike.
//
// Third-party sources (MIT): xBR-lv2 (c) 2011-2022 Hyllian; FidelityFX FSR 1.0.2 (c) 2021 AMD;
// NVIDIA Image Scaling 1.0.2 (c) 2022 NVIDIA CORPORATION & AFFILIATES. Ported from the libretro
// slang shader and agyild's mpv ports, adapted to RGB.
namespace wind {

// Filter ids, the ini key renderFilter. Keep in step with kUpFilterNames and the Settings options.
enum UpFilter : int {
    kUpNearest = 0, kUpBilinear = 1, kUpSharpBilinear = 2, kUpBicubic = 3, kUpLanczos2 = 4,
    kUpLanczos3 = 5, kUpXbr = 6, kUpFsr = 7, kUpNis = 8, kUpRtxVsr = 9, kUpMl = 10, kUpFilterCount = 11
};
inline constexpr const char* kUpFilterNames[kUpFilterCount] = {
    "nearest", "bilinear", "sharp-bilinear", "bicubic", "lanczos2", "lanczos3", "xbr", "fsr", "nis",
    "rtx-vsr", "ml"
};

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

inline constexpr const char* kUpMain = R"(
float4 PSMain(VSOut i) : SV_TARGET {
    float3 c = Upscale(i.uv);
#ifndef UP_NO_COLOR
    c = ApplyColor(saturate(c));
#endif
    return float4(saturate(c), 1.0);
}
)";

// --- 2: sharp bilinear (pixel-art anti-aliasing): crisp texels, only their edges blended.
inline constexpr const char* kUpSrcSharpBilinear = R"(
float3 Upscale(float2 uv) {
    float2 size = SrcSize();
    float2 texel = uv * size;
    float2 fl = floor(texel);
    float2 s = texel - fl;
    float scale = Scale();
    float range = 0.5 - 0.5 / scale;
    float2 cd = s - 0.5;
    float2 f = (cd - clamp(cd, -range, range)) * scale + 0.5;
    return Bilin((fl + f) / size);
}
)";

// --- 3: Catmull-Rom bicubic, 9 bilinear taps (the MJP formulation).
inline constexpr const char* kUpSrcBicubic = R"(
float3 S(float2 uv) { return tex.SampleLevel(smpL, uv, 0).rgb; }
float3 Upscale(float2 uv) {
    float2 size = SrcSize();
    float2 sp = uv * size;
    float2 t1 = floor(sp - 0.5) + 0.5;
    float2 f = sp - t1;
    float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    float2 w3 = f * f * (-0.5 + 0.5 * f);
    float2 w12 = w1 + w2;
    float2 o12 = w2 / w12;
    float2 t0 = (t1 - 1.0) / size, t3 = (t1 + 2.0) / size, t12 = (t1 + o12) / size;
    float3 r = 0;
    r += S(float2(t0.x,  t0.y))  * w0.x  * w0.y;
    r += S(float2(t12.x, t0.y))  * w12.x * w0.y;
    r += S(float2(t3.x,  t0.y))  * w3.x  * w0.y;
    r += S(float2(t0.x,  t12.y)) * w0.x  * w12.y;
    r += S(float2(t12.x, t12.y)) * w12.x * w12.y;
    r += S(float2(t3.x,  t12.y)) * w3.x  * w12.y;
    r += S(float2(t0.x,  t3.y))  * w0.x  * w3.y;
    r += S(float2(t12.x, t3.y))  * w12.x * w3.y;
    r += S(float2(t3.x,  t3.y))  * w3.x  * w3.y;
    return ToDisplay(max(r, 0.0));
}
)";

// --- 4 / 5: Lanczos, radius LZ_R (2 or 3), in display space like DWM.
inline constexpr const char* kUpLanczos = R"(
float Lz(float x) {
    x = abs(x);
    if (x < 1e-5) return 1.0;
    if (x >= LZ_R) return 0.0;
    float px = 3.14159265 * x;
    return LZ_R * sin(px) * sin(px / LZ_R) / (px * px);
}
float3 Upscale(float2 uv) {
    float2 p = uv * SrcSize() - 0.5;
    float2 b = floor(p);
    float2 f = p - b;
    float wx[2 * LZ_R], wy[2 * LZ_R];
    [unroll] for (int k = 0; k < 2 * LZ_R; ++k) {
        wx[k] = Lz(float(k - LZ_R + 1) - f.x);
        wy[k] = Lz(float(k - LZ_R + 1) - f.y);
    }
    float3 acc = 0; float ws = 0;
    [unroll] for (int j = 0; j < 2 * LZ_R; ++j)
        [unroll] for (int i = 0; i < 2 * LZ_R; ++i) {
            float w = wx[i] * wy[j];
            acc += Tap(b + float2(i - LZ_R + 1, j - LZ_R + 1)) * w;
            ws += w;
        }
    return acc / ws;
}
)";

// --- 6: Hyllian's xBR-lv2 (corner C, AA blending), from the libretro standalone shader.
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

// --- 7a: FSR 1 EASU on RGB (luma drives direction and length, RGB is accumulated), into the
// intermediate target; no colour filter here (UP_NO_COLOR), RCAS applies it.
inline constexpr const char* kUpFsrEasu = R"(
#define UP_NO_COLOR 1
float RcpLo(float a) { return asfloat(0x7ef07ebbu - asuint(a)); }
float RsqLo(float a) { return asfloat(0x5f347d74u - (asuint(a) >> 1u)); }
float L(float3 c) { return c.b * 0.5 + (c.r * 0.5 + c.g); }
void Tap12(inout float3 aC, inout float aW, float2 off, float2 dir, float2 len, float lob, float clp, float3 c) {
    float2 v;
    v.x = off.x * dir.x + off.y * dir.y;
    v.y = off.x * (-dir.y) + off.y * dir.x;
    v *= len;
    float d2 = min(v.x * v.x + v.y * v.y, clp);
    float wB = 0.4 * d2 - 1.0;
    float wA = lob * d2 - 1.0;
    wB *= wB; wA *= wA;
    wB = 1.5625 * wB - 0.5625;
    float w = wB * wA;
    aC += c * w; aW += w;
}
void Set(inout float2 dir, inout float len, float w, float lA, float lB, float lC, float lD, float lE) {
    float dc = lD - lC, cb = lC - lB;
    float lenX = max(abs(dc), abs(cb));
    lenX = RcpLo(lenX);
    float dirX = lD - lB;
    lenX = saturate(abs(dirX) * lenX); lenX *= lenX;
    float ec = lE - lC, ca = lC - lA;
    float lenY = max(abs(ec), abs(ca));
    lenY = RcpLo(lenY);
    float dirY = lE - lA;
    lenY = saturate(abs(dirY) * lenY); lenY *= lenY;
    dir += float2(dirX, dirY) * w;
    len += dot(float2(w, w), float2(lenX, lenY));
}
float3 Upscale(float2 uv) {
    float2 pp = uv * SrcSize() - 0.5;
    float2 fp = floor(pp);
    pp -= fp;
    float3 b = Tap(fp + float2(0,-1)), c = Tap(fp + float2(1,-1));
    float3 e = Tap(fp + float2(-1,0)), f = Tap(fp), g = Tap(fp + float2(1,0)), h = Tap(fp + float2(2,0));
    float3 i = Tap(fp + float2(-1,1)), j = Tap(fp + float2(0,1)), k = Tap(fp + float2(1,1)), l = Tap(fp + float2(2,1));
    float3 n = Tap(fp + float2(0,2)), o = Tap(fp + float2(1,2));
    float bL = L(b), cL = L(c), eL = L(e), fL = L(f), gL = L(g), hL = L(h);
    float iL = L(i), jL = L(j), kL = L(k), lL = L(l), nL = L(n), oL = L(o);
    float2 dir = 0; float len = 0;
    Set(dir, len, (1.0 - pp.x) * (1.0 - pp.y), bL, eL, fL, gL, jL);
    Set(dir, len, pp.x * (1.0 - pp.y),         cL, fL, gL, hL, kL);
    Set(dir, len, (1.0 - pp.x) * pp.y,         fL, iL, jL, kL, nL);
    Set(dir, len, pp.x * pp.y,                 gL, jL, kL, lL, oL);
    float2 dir2 = dir * dir;
    float dirR = dir2.x + dir2.y;
    bool zro = dirR < (1.0 / 32768.0);
    dirR = RsqLo(dirR);
    dirR = zro ? 1.0 : dirR;
    dir.x = zro ? 1.0 : dir.x;
    dir *= dirR;
    len = len * 0.5; len *= len;
    float stretch = (dir.x * dir.x + dir.y * dir.y) * RcpLo(max(abs(dir.x), abs(dir.y)));
    float2 len2 = float2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);
    float lob = 0.5 + ((1.0 / 4.0 - 0.04) - 0.5) * len;
    float clp = RcpLo(lob);
    float3 aC = 0; float aW = 0;
    Tap12(aC, aW, float2( 0,-1) - pp, dir, len2, lob, clp, b);
    Tap12(aC, aW, float2( 1,-1) - pp, dir, len2, lob, clp, c);
    Tap12(aC, aW, float2(-1, 1) - pp, dir, len2, lob, clp, i);
    Tap12(aC, aW, float2( 0, 1) - pp, dir, len2, lob, clp, j);
    Tap12(aC, aW, float2( 0, 0) - pp, dir, len2, lob, clp, f);
    Tap12(aC, aW, float2(-1, 0) - pp, dir, len2, lob, clp, e);
    Tap12(aC, aW, float2( 1, 1) - pp, dir, len2, lob, clp, k);
    Tap12(aC, aW, float2( 2, 1) - pp, dir, len2, lob, clp, l);
    Tap12(aC, aW, float2( 2, 0) - pp, dir, len2, lob, clp, h);
    Tap12(aC, aW, float2( 1, 0) - pp, dir, len2, lob, clp, g);
    Tap12(aC, aW, float2( 1, 2) - pp, dir, len2, lob, clp, o);
    Tap12(aC, aW, float2( 0, 2) - pp, dir, len2, lob, clp, n);
    float3 pix = aC / aW;
    float3 mn = min(min(f, g), min(j, k)), mx = max(max(f, g), max(j, k));
    return clamp(pix, mn, mx);
}
)";

// --- 7b: FSR 1 RCAS (sharpen 0.2 stops, denoise on) over the EASU output at output resolution,
// then the colour filter and brightness. Its own complete shader (reads the intermediate as tex).
inline constexpr const char* kUpFsrRcas = R"(
float RcpMed(float a) { float b = asfloat(0x7ef19fffu - asuint(a)); return b * (-b * a + 2.0); }
float Max3(float x, float y, float z) { return max(x, max(y, z)); }
float Min3(float x, float y, float z) { return min(x, min(y, z)); }
float4 PSMain(VSOut vin) : SV_TARGET {
    int2 p = int2(vin.pos.xy);
    float3 b = tex.Load(int3(p + int2(0,-1), 0)).rgb;
    float3 d = tex.Load(int3(p + int2(-1,0), 0)).rgb;
    float3 e = tex.Load(int3(p, 0)).rgb;
    float3 f = tex.Load(int3(p + int2(1,0), 0)).rgb;
    float3 h = tex.Load(int3(p + int2(0,1), 0)).rgb;
    float bL = b.b * 0.5 + (b.r * 0.5 + b.g), dL = d.b * 0.5 + (d.r * 0.5 + d.g);
    float eL = e.b * 0.5 + (e.r * 0.5 + e.g), fL = f.b * 0.5 + (f.r * 0.5 + f.g);
    float hL = h.b * 0.5 + (h.r * 0.5 + h.g);
    float nz = 0.25 * bL + 0.25 * dL + 0.25 * fL + 0.25 * hL - eL;
    nz = saturate(abs(nz) * RcpMed(Max3(Max3(bL, dL, eL), fL, hL) - Min3(Min3(bL, dL, eL), fL, hL)));
    nz = -0.5 * nz + 1.0;
    float3 mn4 = min(min(b, d), min(f, h));
    float3 mx4 = max(max(b, d), max(f, h));
    float2 peakC = float2(1.0, -4.0);
    float3 hitMin = min(mn4, e) / (4.0 * mx4);
    float3 hitMax = (peakC.x - max(mx4, e)) / (4.0 * mn4 + peakC.y);
    float3 lobeRGB = max(-hitMin, hitMax);
    float lobe = max(-(0.25 - (1.0 / 16.0)), min(Max3(lobeRGB.r, lobeRGB.g, lobeRGB.b), 0.0)) * exp2(-0.2);
    lobe *= nz;
    float rcpL = RcpMed(4.0 * lobe + 1.0);
    float3 c = (lobe * b + lobe * d + lobe * h + lobe * f + e) * rcpL;
    return float4(saturate(ApplyColor(saturate(c))), 1.0);
}
)";

// --- 9 / 10: the final pass for an AI upscaler (RTX VSR, the ML model). Its output covers a whole-
// texel region at k times the source size; uvMin/uvMax (with kUpViewVS) pick the visible part of
// it at sub-texel precision, sampled linearly, then the colour filter and brightness.
inline constexpr const char* kUpBlit = R"(
float4 PSMain(VSOut vin) : SV_TARGET {
    float3 c = tex.SampleLevel(smpL, vin.uv, 0).rgb;
    return float4(saturate(ApplyColor(saturate(c))), 1.0);
}
)";

// The AI upscalers' input: the source region tonemapped into 8-bit (VSR takes 8/10-bit only, and
// the model wants display space). Drawn with kUpFullVS into a region-sized target; uvMin holds the
// region's top-left texel.
inline constexpr const char* kUpSrcCopy = R"(
float4 PSMain(VSOut vin) : SV_TARGET {
    return float4(Tap(floor(vin.pos.xy) + uvMin), 1.0);
}
)";

// The vertex shader for the final passes: plain full-screen UV, uvMin/uvMax ignored.
inline constexpr const char* kUpFullVS = R"(
VSOut VSMain(uint id : SV_VertexID) {
    float2 t = float2((id << 1) & 2, id & 2);
    VSOut o;
    o.pos = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv  = t;
    return o;
}
)";
// The vertex shader for the upscale passes: maps the screen onto the visible source rect.
inline constexpr const char* kUpViewVS = R"(
VSOut VSMain(uint id : SV_VertexID) {
    float2 t = float2((id << 1) & 2, id & 2);
    VSOut o;
    o.pos = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv  = lerp(uvMin, uvMax, t);
    return o;
}
)";

}  // namespace wind
