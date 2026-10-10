#pragma once
// Smooth-zoom ladder (issue #369) - PURE, no <windows.h>, tests/test_zoom_ladder.cpp.
//
// DWM's smooth sampling renders the zoomed subtree into a scratch image whose SIZE and ORIGIN are
// rounded to whole pixels every frame (CResampleLayer::Create, PushEffects/PixelAlign; see the
// compositor notes), then stretches it to the screen. During a continuous zoom that rounding moves
// the image by a different amount each frame: the zoom "shake". Measured live (3840x2160, pointer
// fixed): cursor-tip jitter 4.7 px p95 at 2-10x and 11-12 px at 10-25x, jumps up to 42 px. Two closed
// terms predict it per axis, with no fitted factors:
//     error = c * (S/z - n)/n  -  (round(o) - o) * z,     n = round(S/z)
// (S screen extent, z level, o the clamped source origin, c the pointer's screen position). Zooming
// only through levels where both axes predict under 1 px measured 0.5-0.8 px p95 and 1-2 px jumps at
// 2-25x, as steady as nearest sampling. Such levels lie under 0.9 % apart up to 12x (about 4 % at
// 12-25x), finer than a zoom tick, so a ramp snapped to them still reads as continuous.
#include <cmath>
namespace wind {

inline double LadderOrigin(double centre, double z, int extent) {
    double o = centre - extent / (2.0 * z);
    const double hi = extent - extent / z;
    if (o > hi) o = hi;
    if (o < 0.0) o = 0.0;
    return o;
}

// Predicted smooth-sampling displacement (screen px) on one axis for a view centred on `centre`.
inline double SmoothRoundingError(double z, int extent, double centre) {
    double n = std::floor(extent / z + 0.5);
    if (n < 1.0) n = 1.0;
    const double o = LadderOrigin(centre, z, extent);
    const double c = (centre - o) * z;   // pointer screen position
    return c * (extent / z - n) / n - (std::floor(o + 0.5) - o) * z;
}

// Worst predicted displacement on one axis over the pointer and the middle half of the screen.
// The size-ratio term grows linearly with the screen position, so a level that is clean at the
// pointer can still move content elsewhere by several px (field: a horizontal line shook while
// the cursor was steady; up to 6 px predicted). The middle half is where the eye is; requiring
// the whole screen leaves 5-11 % gaps between clean levels above 12x. Predicted p95 over a sweep:
// pointer-only rule 3.2 px in the middle half / 6.4 px at the edges; this rule 1.0 / 2.0 px.
inline double SmoothRoundingErrorArea(double z, int extent, double centre) {
    double n = std::floor(extent / z + 0.5);
    if (n < 1.0) n = 1.0;
    const double o = LadderOrigin(centre, z, extent);
    const double slope = (extent / z - n) / n;
    const double shift = (std::floor(o + 0.5) - o) * z;
    const double cPtr = (centre - o) * z;
    double worst = std::fabs(slope * cPtr - shift);
    const double lo = std::fabs(slope * extent * 0.25 - shift), hi = std::fabs(slope * extent * 0.75 - shift);
    if (lo > worst) worst = lo;
    if (hi > worst) worst = hi;
    return worst;
}

// Allowed predicted error: 1 px up to 12x (clean levels at most 1.1 % apart), then growing slowly
// (1.4 px at 20x, 2 px at 32x) so the clean levels stay at most 3.1 % apart at 12-20x and 4.7 % above,
// where one source pixel already spans 12-30 screen px.
inline double SmoothLadderTolerance(double z) {
    const double t = 0.05 * z + 0.4;
    return (z <= 12.0 || t < 1.0) ? 1.0 : t;
}

// The level nearest `want` (within +-maxRel) whose predicted error over the pointer and the middle
// half of the screen is under tolerance on both axes; `want` itself when none is found. Never
// steps behind `floorLevel` in the ramp's direction (dir > 0 zooming in, < 0 out, 0 settled), so a
// snapped ramp cannot visibly reverse.
// How far from the request a snap may go. Field 2026-10-07: up to 5 % made zooms jump forwards and
// back; a flat 0.5 % found no clean level at 20-31x (predicted shake back to 15 px p95). Clean
// levels thin out with zoom, so the window grows with it: 0.5 % up to 12x, then to 1.2 % at 24x and
// above (predicted at 20-31x: 0 px at the pointer, under 3 px over the middle half).
inline double SnapWindow(double z) {
    if (z <= 12.0) return 0.005;
    const double w = 0.005 + (z - 12.0) * (0.007 / 12.0);
    return w > 0.012 ? 0.012 : w;
}

// Tiered: the middle half of the screen clean within 0.5 %, else the pointer clean within
// SnapWindow, else the request itself (that frame may shake a little; the zoom never jumps).
// stepRel (> 0) caps both windows at the zoom's own motion this frame: a fast zoom has room to
// pick clean levels, a decelerating one almost none, so the ease-out after the key is released
// glides out exactly instead of holding a level and hopping to the next (field 2026-10-07: the
// zoom visibly "settled" after the key was released).
inline double SnapSmoothLevel(double want, double centreX, double centreY, int w, int h,
                              double floorLevel = 0.0, int dir = 0, double maxRel = -1.0,
                              double stepRel = -1.0) {
    if (want <= 1.001 || w <= 0 || h <= 0) return want;
    auto areaOk = [&](double z) {
        const double tol = SmoothLadderTolerance(z);
        return SmoothRoundingErrorArea(z, w, centreX) < tol &&
               SmoothRoundingErrorArea(z, h, centreY) < tol;
    };
    auto ptrOk = [&](double z) {
        return std::fabs(SmoothRoundingError(z, w, centreX)) < 1.0 &&
               std::fabs(SmoothRoundingError(z, h, centreY)) < 1.0;
    };
    auto allowed = [&](double z) {
        if (z <= 1.001) return false;
        if (dir > 0 && floorLevel > 0.0 && z < floorLevel) return false;
        if (dir < 0 && floorLevel > 0.0 && z > floorLevel) return false;
        return true;
    };
    const double step = want * 1e-5;
    for (int pass = 0; pass < 2; ++pass) {
        double rel = maxRel > 0.0 ? maxRel : (pass == 0 ? 0.005 : SnapWindow(want));
        if (stepRel > 0.0 && stepRel < rel) rel = stepRel;
        const int steps = (int)(rel / 1e-5);
        auto ok = [&](double z) { return pass == 0 ? areaOk(z) : ptrOk(z); };
        if (allowed(want) && ok(want)) return want;
        for (int i = 1; i <= steps; ++i) {
            const double a = want + i * step, b = want - i * step;
            if (allowed(a) && ok(a)) return a;
            if (allowed(b) && ok(b)) return b;
        }
    }
    return want;
}

// TRUNCATED EASE-OUT (field 2026-10-07). After a release the user's ease-out runs, snapped to
// clean levels like a held zoom; once it moves less per frame than clean levels are apart, the
// ladder could only hop or jump, so the zoom stops there on the level on screen. The fast part of
// the glide (nearly all of it) survives; only the barely-moving tail is cut.
inline bool EaseOutShouldStop(double level, double prevLevel) {
    if (level <= 1.001 || prevLevel <= 1.001) return false;
    const double step = std::fabs(level - prevLevel) / level;
    return step < 0.6 * SnapWindow(level);
}

}  // namespace wind
