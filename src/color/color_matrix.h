// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
#pragma once
// Colour filter matrices (issue #288). Pure; tests/test_color_matrix.cpp.
//
// Layout matches the DWM colour effect (MAGCOLOREFFECT): a ROW vector [R G B A 1] times the 5x5
// matrix, so out[j] = sum_i in[i] * m[i][j] and row 4 holds the offsets. The same numbers feed the
// render engine's pixel shader, which cannot use the DWM effect (its capture already contains it:
// docs/COLOUR-FILTER-FINDINGS.md).
#include <cmath>
namespace wind {

struct ColorMatrix { float m[5][5]; };

inline ColorMatrix IdentityColorMatrix() {
    ColorMatrix r{};
    for (int i = 0; i < 5; ++i) r.m[i][i] = 1.0f;
    return r;
}

inline bool IsIdentity(const ColorMatrix& c) {
    for (int i = 0; i < 5; ++i)
        for (int j = 0; j < 5; ++j) {
            const float want = i == j ? 1.0f : 0.0f;
            const float d = c.m[i][j] - want;
            if (d > 1e-6f || d < -1e-6f) return false;
        }
    return true;
}

inline bool SameMatrix(const ColorMatrix& a, const ColorMatrix& b) {
    for (int i = 0; i < 5; ++i)
        for (int j = 0; j < 5; ++j)
            if (a.m[i][j] != b.m[i][j]) return false;
    return true;
}

// Lowest brightness the dim control reaches: 1% (owner, 2026-09-30: 0 made the screen completely
// black, so the floor stays just above it; under HDR 1% decodes to ~0.08% light, still readable on an
// OLED in the dark). Quitting Wind (Win+Esc) always clears the effect; so does a crash.
inline constexpr double kMinDim01 = 0.01;

// Channel gains for a blackbody colour temperature, in LINEAR light, normalised so 6500 K is white
// and the largest channel is 1. Computed from Planck's law and the CIE 1931 colour-matching functions
// (Wyman-Sloan-Shirley fit), converted to linear sRGB; 1000 K to 6500 K in 100 K steps.
inline constexpr float kKelvinGains[][3] = {
    { 1.0000f, 0.0303f, 0.0000f }, { 1.0000f, 0.0532f, 0.0000f }, { 1.0000f, 0.0774f, 0.0000f }, { 1.0000f, 0.1024f, 0.0000f },
    { 1.0000f, 0.1279f, 0.0000f }, { 1.0000f, 0.1537f, 0.0000f }, { 1.0000f, 0.1796f, 0.0000f }, { 1.0000f, 0.2055f, 0.0000f },
    { 1.0000f, 0.2312f, 0.0000f }, { 1.0000f, 0.2567f, 0.0000f }, { 1.0000f, 0.2820f, 0.0077f }, { 1.0000f, 0.3069f, 0.0174f },
    { 1.0000f, 0.3315f, 0.0283f }, { 1.0000f, 0.3557f, 0.0404f }, { 1.0000f, 0.3794f, 0.0538f }, { 1.0000f, 0.4027f, 0.0682f },
    { 1.0000f, 0.4256f, 0.0838f }, { 1.0000f, 0.4480f, 0.1004f }, { 1.0000f, 0.4700f, 0.1179f }, { 1.0000f, 0.4915f, 0.1364f },
    { 1.0000f, 0.5125f, 0.1557f }, { 1.0000f, 0.5331f, 0.1757f }, { 1.0000f, 0.5532f, 0.1965f }, { 1.0000f, 0.5729f, 0.2179f },
    { 1.0000f, 0.5920f, 0.2400f }, { 1.0000f, 0.6108f, 0.2625f }, { 1.0000f, 0.6291f, 0.2856f }, { 1.0000f, 0.6469f, 0.3091f },
    { 1.0000f, 0.6643f, 0.3330f }, { 1.0000f, 0.6813f, 0.3572f }, { 1.0000f, 0.6979f, 0.3817f }, { 1.0000f, 0.7140f, 0.4064f },
    { 1.0000f, 0.7298f, 0.4314f }, { 1.0000f, 0.7451f, 0.4565f }, { 1.0000f, 0.7601f, 0.4818f }, { 1.0000f, 0.7747f, 0.5072f },
    { 1.0000f, 0.7889f, 0.5326f }, { 1.0000f, 0.8028f, 0.5581f }, { 1.0000f, 0.8163f, 0.5836f }, { 1.0000f, 0.8295f, 0.6091f },
    { 1.0000f, 0.8423f, 0.6346f }, { 1.0000f, 0.8548f, 0.6599f }, { 1.0000f, 0.8670f, 0.6852f }, { 1.0000f, 0.8789f, 0.7104f },
    { 1.0000f, 0.8904f, 0.7355f }, { 1.0000f, 0.9017f, 0.7605f }, { 1.0000f, 0.9127f, 0.7853f }, { 1.0000f, 0.9234f, 0.8099f },
    { 1.0000f, 0.9339f, 0.8344f }, { 1.0000f, 0.9441f, 0.8587f }, { 1.0000f, 0.9540f, 0.8828f }, { 1.0000f, 0.9637f, 0.9067f },
    { 1.0000f, 0.9731f, 0.9303f }, { 1.0000f, 0.9823f, 0.9538f }, { 1.0000f, 0.9913f, 0.9770f }, { 1.0000f, 1.0000f, 1.0000f }
};
inline constexpr int kKelvinMin = 1000, kKelvinStep = 100;
inline constexpr int kKelvinCount = (int)(sizeof(kKelvinGains) / sizeof(kKelvinGains[0]));

inline void KelvinGainsLinear(double kelvin, double& r, double& g, double& b) {
    double f = (kelvin - kKelvinMin) / kKelvinStep;
    if (f < 0) f = 0;
    if (f > kKelvinCount - 1) f = kKelvinCount - 1;
    const int i = (int)f;
    const int j = i + 1 < kKelvinCount ? i + 1 : i;
    const double t = f - i;
    r = kKelvinGains[i][0] + (kKelvinGains[j][0] - kKelvinGains[i][0]) * t;
    g = kKelvinGains[i][1] + (kKelvinGains[j][1] - kKelvinGains[i][1]) * t;
    b = kKelvinGains[i][2] + (kKelvinGains[j][2] - kKelvinGains[i][2]) * t;
}

inline double SrgbEncode(double c) { return c <= 0.0031308 ? 12.92 * c : 1.055 * std::pow(c, 1.0 / 2.4) - 0.055; }
inline double SrgbDecode(double c) { return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); }

// Warmth 0..1, two halves (owner 2026-11-21: "make it go further", the old 100% now at 50%).
// 0..0.5 is Night light's own scale stretched over half the slider: 0% = 6500 K, 50% = 1200 K,
// LINEAR in Kelvin (Night light measured 2026-09-29 from its stored setting: its 50% = 3850 K).
// 0.5..1 goes past where the blackbody table ends: the green 1200 K leaves fades out evenly in
// sRGB-encoded terms (what the eye sees), so 100% is pure red. Blue is already 0 at 1200 K.
inline constexpr double kWarmKelvinEnd = 1200.0;
inline double WarmKelvin(double warm01) {
    const double h = warm01 >= 0.5 ? 1.0 : warm01 * 2.0;
    return 6500.0 - (6500.0 - kWarmKelvinEnd) * h;
}
inline void WarmGainsLinear(double warm01, double& r, double& g, double& b) {
    KelvinGainsLinear(WarmKelvin(warm01), r, g, b);
    if (warm01 <= 0.5) return;
    const double t = warm01 >= 1.0 ? 1.0 : (warm01 - 0.5) * 2.0;
    g = SrgbDecode(SrgbEncode(g) * (1.0 - t));
    b = SrgbDecode(SrgbEncode(b) * (1.0 - t));
}

// The colour feature is warmth and brightness (owner decision 2026-09-29) plus saturation (owner
// 2026-11-21, floored at 20%). Invert, greyscale and two-colour tints were dropped: one colour matrix
// cannot keep multi-coloured text readable (docs/COLOUR-FILTER-FINDINGS.md).
// warm01: 0..1 (0 = no warmth). dim01: kMinDim01..1 (1 = no dim). Out-of-range values clamp.
// linearLight: the matrix acts on LINEAR values. True for the DWM colour effect while Windows HDR is
// on (measured: it scales scRGB directly); false for SDR DWM and for the render engine's shader,
// which apply it to sRGB-encoded values. Warmth uses the blackbody gains in the matching space, and
// brightness is decoded to linear there, so both look the same in SDR and HDR.
// Both at their neutral ends give the identity, which the controller treats as "off".
// sat01: saturation, kMinSat01..kMaxSat01 (1 = unchanged), applied FIRST: each channel moves toward
// (below 1) or away from (above 1) the pixel's Rec.709 luma, then warmth and dim scale the channels.
// Same weights in both spaces (on encoded values it is the usual CSS saturate() approximation).
// The floor is not 0: greyscale merges text colours of equal brightness (COLOUR-FILTER-FINDINGS.md).
inline constexpr double kMinSat01 = 0.2, kMaxSat01 = 1.5;
inline constexpr double kLumaR = 0.2126, kLumaG = 0.7152, kLumaB = 0.0722;
inline ColorMatrix BuildColorMatrix(double warm01, double dim01, bool linearLight, double sat01 = 1.0) {
    auto clamp = [](double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); };
    warm01 = clamp(warm01, 0.0, 1.0);
    dim01 = clamp(dim01, kMinDim01, 1.0);
    sat01 = clamp(sat01, kMinSat01, kMaxSat01);
    double gain[3] = { 1.0, 1.0, 1.0 };
    if (warm01 > 0.0) {
        double g[3];
        WarmGainsLinear(warm01, g[0], g[1], g[2]);
        for (int i = 0; i < 3; ++i) gain[i] = linearLight ? g[i] : SrgbEncode(g[i]);
    }
    const double d = linearLight ? SrgbDecode(dim01) : dim01;
    for (int i = 0; i < 3; ++i) gain[i] *= d;                          // dim composes last
    // Row vectors: out = in * S * diag(gain), so m[i][j] = S[i][j] * gain[j].
    const double luma[3] = { kLumaR, kLumaG, kLumaB };
    ColorMatrix m = IdentityColorMatrix();
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            const double s = (1.0 - sat01) * luma[i] + (i == j ? sat01 : 0.0);
            m.m[i][j] = (float)(s * gain[j]);
        }
    return m;
}

// Test/diagnostic helper: apply to an opaque RGB colour.
inline void ApplyToRgb(const ColorMatrix& c, double r, double g, double b, double& o0, double& o1, double& o2) {
    const double in[5] = { r, g, b, 1.0, 1.0 };
    double out[3] = { 0, 0, 0 };
    for (int j = 0; j < 3; ++j)
        for (int i = 0; i < 5; ++i) out[j] += in[i] * c.m[i][j];
    o0 = out[0]; o1 = out[1]; o2 = out[2];
}
}  // namespace wind
