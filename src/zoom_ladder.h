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

// The level nearest `want` (within +-maxRel) whose predicted error is under tol on both axes;
// `want` itself when none is found. Never steps behind `floorLevel` in the ramp's direction
// (dir > 0 zooming in, < 0 out, 0 settled), so a snapped ramp cannot visibly reverse.
inline double SnapSmoothLevel(double want, double centreX, double centreY, int w, int h,
                              double floorLevel = 0.0, int dir = 0,
                              double tol = 1.0, double maxRel = 0.02) {
    if (want <= 1.001 || w <= 0 || h <= 0) return want;
    auto ok = [&](double z) {
        return std::fabs(SmoothRoundingError(z, w, centreX)) < tol &&
               std::fabs(SmoothRoundingError(z, h, centreY)) < tol;
    };
    auto allowed = [&](double z) {
        if (z <= 1.001) return false;
        if (dir > 0 && floorLevel > 0.0 && z < floorLevel) return false;
        if (dir < 0 && floorLevel > 0.0 && z > floorLevel) return false;
        return true;
    };
    const double step = want * 1e-5;
    const int steps = (int)(maxRel / 1e-5);
    if (allowed(want) && ok(want)) return want;
    for (int i = 1; i <= steps; ++i) {
        const double a = want + i * step, b = want - i * step;
        if (allowed(a) && ok(a)) return a;
        if (allowed(b) && ok(b)) return b;
    }
    return want;
}

}  // namespace wind
