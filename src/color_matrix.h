#pragma once
// Colour filter matrices (issue #288). Pure; tests/test_color_matrix.cpp.
//
// Layout matches the DWM colour effect (MAGCOLOREFFECT): a ROW vector [R G B A 1] times the 5x5
// matrix, so out[j] = sum_i in[i] * m[i][j] and row 4 holds the offsets. The same numbers feed the
// render engine's pixel shader, which cannot use the DWM effect (its capture already contains it:
// docs/COLOUR-FILTER-FINDINGS.md).
namespace wind {

enum class ColorFilter { Off = 0, Invert, Greyscale, Warm, YellowOnBlack, WhiteOnBlue, GreenOnBlack };

struct ColorMatrix { float m[5][5]; };

inline ColorMatrix IdentityColorMatrix() {
    ColorMatrix r{};
    for (int i = 0; i < 5; ++i) r.m[i][i] = 1.0f;
    return r;
}

inline ColorMatrix Multiply(const ColorMatrix& a, const ColorMatrix& b) {
    ColorMatrix r{};
    for (int i = 0; i < 5; ++i)
        for (int j = 0; j < 5; ++j) {
            double s = 0.0;
            for (int k = 0; k < 5; ++k) s += (double)a.m[i][k] * b.m[k][j];
            r.m[i][j] = (float)s;
        }
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

// warm01: Warm strength 0..1. dim01: brightness 0.2..1 (1 = no dim). Out-of-range values clamp.
inline ColorMatrix BuildColorMatrix(ColorFilter f, double warm01, double dim01) {
    auto clamp = [](double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); };
    warm01 = clamp(warm01, 0.0, 1.0);
    dim01 = clamp(dim01, 0.2, 1.0);
    const double w[3] = { 0.2126, 0.7152, 0.0722 };   // Rec.709 luma
    ColorMatrix m = IdentityColorMatrix();
    auto twoColour = [&](const double fg[3], const double bg[3]) {
        // Inverted luma drives the blend, so dark text on a light page becomes fg on bg:
        // out = bg + (1 - L) (fg - bg) = fg - L (fg - bg).
        ColorMatrix t = IdentityColorMatrix();
        for (int j = 0; j < 3; ++j) {
            for (int i = 0; i < 3; ++i) t.m[i][j] = (float)(-w[i] * (fg[j] - bg[j]));
            t.m[4][j] = (float)fg[j];
        }
        return t;
    };
    switch (f) {
        case ColorFilter::Invert:
            for (int i = 0; i < 3; ++i) { m.m[i][i] = -1.0f; m.m[4][i] = 1.0f; }
            break;
        case ColorFilter::Greyscale:
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j) m.m[i][j] = (float)w[i];
            break;
        case ColorFilter::Warm:
            m.m[1][1] = (float)(1.0 - 0.25 * warm01);
            m.m[2][2] = (float)(1.0 - 0.6 * warm01);
            break;
        case ColorFilter::YellowOnBlack: { const double fg[3] = { 1, 1, 0 }, bg[3] = { 0, 0, 0 }; m = twoColour(fg, bg); break; }
        case ColorFilter::WhiteOnBlue:   { const double fg[3] = { 1, 1, 1 }, bg[3] = { 0, 0, 0.5 }; m = twoColour(fg, bg); break; }
        case ColorFilter::GreenOnBlack:  { const double fg[3] = { 0, 1, 0 }, bg[3] = { 0, 0, 0 }; m = twoColour(fg, bg); break; }
        default: break;   // Off, or an unknown value from a hand-edited ini
    }
    if (dim01 < 1.0) {
        ColorMatrix d = IdentityColorMatrix();
        for (int i = 0; i < 3; ++i) d.m[i][i] = (float)dim01;
        m = Multiply(m, d);   // dim last: out = (in x filter) x dim
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
