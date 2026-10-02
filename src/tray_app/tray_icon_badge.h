#pragma once
// The tray icon's state badges (#315), painted into the logo's own pixels so the shell needs no
// second icon: a teal dot that pulses while a Mouse lock / Pass keys chip is listening, and a pause
// mark while Wind is paused. PURE (no <windows.h>): straight-alpha BGRA in, straight-alpha BGRA out,
// unit-tested. Each badge sits on a dark disc so it reads on any taskbar colour.
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace wind { namespace TrayBadge {

// Brightness of the dot at pulse frame `frame` of `frames`: a soft cosine between 0.35 and 1.
inline float PulseAmount(int frame, int frames) {
    if (frames <= 0) return 1.f;
    const double ph = (double)((frame % frames + frames) % frames) / (double)frames;
    return (float)(0.35 + 0.65 * (0.5 - 0.5 * std::cos(ph * 6.283185307179586)));
}

namespace detail {
// Source-over of colour (r,g,b) at alpha `a` onto straight-alpha BGRA pixel `p`.
inline void Over(unsigned char* p, int r, int g, int b, float a) {
    if (a <= 0.f) return;
    const float da = p[3] / 255.f;
    const float oa = a + da * (1.f - a);
    if (oa <= 0.f) return;
    const float dst[3] = { (float)p[0], (float)p[1], (float)p[2] };
    const float src[3] = { (float)b, (float)g, (float)r };
    for (int i = 0; i < 3; ++i)
        p[i] = (unsigned char)(std::min)(255.f, (src[i] * a + dst[i] * da * (1.f - a)) / oa + .5f);
    p[3] = (unsigned char)(std::min)(255.f, oa * 255.f + .5f);
}
// Coverage 0..1 of one pixel by `inside(x, y)`, 4x4 supersampled.
template <class F> float Coverage(int x, int y, F inside) {
    int n = 0;
    for (int sy = 0; sy < 4; ++sy)
        for (int sx = 0; sx < 4; ++sx)
            if (inside((float)x + (sx + .5f) / 4.f, (float)y + (sy + .5f) / 4.f)) ++n;
    return n / 16.f;
}
}  // namespace detail

// Paints the badge(s) into a w x h straight-alpha BGRA image. `pause`: dark disc + two bars at the
// bottom right. `dot`: dark disc + teal dot at `dotAmount` brightness; at the bottom right, or the
// top right when the pause mark already holds the bottom right.
inline void Paint(unsigned char* bgra, int w, int h, bool pause, bool dot, float dotAmount) {
    if (!bgra || w < 8 || h < 8) return;
    const float r = (std::max)(3.f, (float)(std::min)(w, h) * 0.25f);
    const float cx = (float)w - r - 0.5f;
    const float cyBottom = (float)h - r - 0.5f, cyTop = r + 0.5f;
    auto disc = [&](float ccx, float ccy, float rad) {
        return [=](float x, float y) { return (x - ccx) * (x - ccx) + (y - ccy) * (y - ccy) <= rad * rad; };
    };
    auto paintAt = [&](auto&& fn, int x0, int y0, int x1, int y1) {
        for (int y = (std::max)(0, y0); y < (std::min)(h, y1); ++y)
            for (int x = (std::max)(0, x0); x < (std::min)(w, x1); ++x)
                fn(bgra + ((size_t)y * w + x) * 4, x, y);
    };
    auto box = [&](float ccy, auto&& fn) {
        paintAt(fn, (int)(cx - r) - 1, (int)(ccy - r) - 1, (int)(cx + r) + 2, (int)(ccy + r) + 2);
    };
    if (pause) {
        const float cy = cyBottom;
        box(cy, [&](unsigned char* p, int x, int y) {
            detail::Over(p, 14, 16, 18, 0.92f * detail::Coverage(x, y, disc(cx, cy, r)));
        });
        const float bw = (std::max)(1.f, r * 0.34f), gap = r * 0.30f, bh = r * 1.05f;
        const float l0 = cx - gap - bw, l1 = cx - gap, r0 = cx + gap, r1 = cx + gap + bw;
        box(cy, [&](unsigned char* p, int x, int y) {
            detail::Over(p, 242, 242, 242, detail::Coverage(x, y, [&](float fx, float fy) {
                return fy >= cy - bh / 2 && fy <= cy + bh / 2 && ((fx >= l0 && fx <= l1) || (fx >= r0 && fx <= r1));
            }));
        });
    }
    if (dot) {
        const float cy = pause ? cyTop : cyBottom;
        const float amt = (std::min)(1.f, (std::max)(0.f, dotAmount));
        box(cy, [&](unsigned char* p, int x, int y) {
            detail::Over(p, 14, 16, 18, 0.82f * detail::Coverage(x, y, disc(cx, cy, r)));
        });
        box(cy, [&](unsigned char* p, int x, int y) {
            detail::Over(p, 0x2f, 0xbf, 0xa5, amt * detail::Coverage(x, y, disc(cx, cy, (std::max)(1.5f, r - 1.25f))));
        });
    }
}

}}  // namespace wind::TrayBadge
