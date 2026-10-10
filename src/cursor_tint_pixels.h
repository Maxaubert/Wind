#pragma once
// Pixel work for the tinted 1x pointer (spec 2026-09-30-cursor-tint-design.md). Pure; tests in
// tests/test_cursor_tint.cpp. Pixels are 32bpp BI_RGB DIB values: 0xAARRGGBB, straight (not
// premultiplied) alpha, which is what cursor colour bitmaps hold and CreateIconIndirect takes.
#include <cstdint>
#include "color_matrix.h"
namespace wind {

inline uint32_t TintPixel(uint32_t p, const ColorMatrix& m) {
    const double r = ((p >> 16) & 0xFF) / 255.0, g = ((p >> 8) & 0xFF) / 255.0, b = (p & 0xFF) / 255.0;
    double o[3];
    ApplyToRgb(m, r, g, b, o[0], o[1], o[2]);
    uint32_t c[3];
    for (int i = 0; i < 3; ++i) {
        double v = o[i] < 0 ? 0 : (o[i] > 1 ? 1 : o[i]);
        c[i] = (uint32_t)(v * 255.0 + 0.5);
    }
    return (p & 0xFF000000u) | (c[0] << 16) | (c[1] << 8) | c[2];
}

inline void TintArgb(uint32_t* px, int n, const ColorMatrix& m) {
    for (int i = 0; i < n; ++i) px[i] = TintPixel(px[i], m);
}

// An old-style colour cursor carries no alpha (all zero) and relies on its AND mask instead.
inline bool AnyAlpha(const uint32_t* px, int n) {
    for (int i = 0; i < n; ++i) if (px[i] & 0xFF000000u) return true;
    return false;
}

// 1bpp DIB rows, DWORD-aligned, most significant bit = leftmost pixel.
inline bool MaskBit(const uint8_t* bits, int stride, int x, int y) {
    return (bits[y * stride + (x >> 3)] >> (7 - (x & 7))) & 1;
}

// Alpha from an AND mask: bit 1 = transparent (screen shows through), bit 0 = opaque.
inline void AlphaFromMask(uint32_t* px, int w, int h, const uint8_t* andBits, int stride) {
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint32_t& p = px[y * w + x];
            p = (p & 0x00FFFFFFu) | (MaskBit(andBits, stride, x, y) ? 0u : 0xFF000000u);
        }
}

// True when a monochrome pointer (AND + XOR masks) has at least one "invert the screen" pixel
// (AND 1, XOR 1) and no opaque black/white pixel (AND 0). Such a pointer (the classic text beam)
// is drawn with the invert blend instead of as opaque pixels.
inline bool MonoIsPureInvert(const uint8_t* andBits, const uint8_t* xorBits, int stride, int w, int h) {
    bool anyInvert = false;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            if (!MaskBit(andBits, stride, x, y)) return false;
            if (MaskBit(xorBits, stride, x, y)) anyInvert = true;
        }
    return anyInvert;
}

// A monochrome pointer (AND + XOR masks) as a colour one:
//   AND 0 XOR 0 -> black, AND 0 XOR 1 -> white, AND 1 XOR 0 -> transparent,
//   AND 1 XOR 1 -> "invert the screen": drawn white, and every transparent pixel touching one
//   (8-neighbourhood) becomes a black outline, so the beam reads on light and dark backgrounds.
// Inverting cannot be tinted (it is not a colour), so this is the price of tinting it.
inline void MonoToArgb(const uint8_t* andBits, const uint8_t* xorBits, int stride, int w, int h, uint32_t* out) {
    auto inv = [&](int x, int y) {
        return x >= 0 && y >= 0 && x < w && y < h && MaskBit(andBits, stride, x, y) && MaskBit(xorBits, stride, x, y);
    };
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const bool a = MaskBit(andBits, stride, x, y), xo = MaskBit(xorBits, stride, x, y);
            uint32_t p;
            if (!a) p = xo ? 0xFFFFFFFFu : 0xFF000000u;
            else if (xo) p = 0xFFFFFFFFu;
            else {
                bool edge = false;
                for (int dy = -1; dy <= 1 && !edge; ++dy)
                    for (int dx = -1; dx <= 1 && !edge; ++dx)
                        if ((dx || dy) && inv(x + dx, y + dy)) edge = true;
                p = edge ? 0xFF000000u : 0x00000000u;
            }
            out[y * w + x] = p;
        }
}
}  // namespace wind
