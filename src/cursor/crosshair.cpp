#include "crosshair.h"
#include <cmath>
namespace wind {

// The single source of the Inspect crosshair design (moved verbatim from render_engine's inline
// texture build). Thin full-length cross: the vertical and horizontal lines run continuously
// through the center (no gap). Light-grey core + black outline so it reads on any background.
// Tuned with sub-pixel precision via 4x4 supersampled coverage: a hard on/off sprite at the
// half-pixel center quantizes thickness to 2px steps, too coarse. Each texel's alpha = covered
// fraction, and its grey/black mix = core-vs-outline coverage, so edges anti-alias cleanly under
// the alpha blend. The arm geometry is FIXED (not scaled by n), so a larger bitmap only adds
// transparent margin around the same cross.
std::vector<uint32_t> BuildCrosshairBGRA(int n, bool premultiply) {
    const double c = (n - 1) * 0.5;   // half-pixel center (23.5 for the classic 48x48)
    const double armLen = 22.5, gap = 0.0, coreHalf = 1.25, outlineHalf = 2.35;
    const int SS = 4; const double inv = 1.0 / SS;
    std::vector<uint32_t> px((size_t)n * n, 0);
    for (int yy = 0; yy < n; ++yy) for (int xx = 0; xx < n; ++xx) {
        int covered = 0, coreCov = 0;
        for (int sy = 0; sy < SS; ++sy) for (int sx = 0; sx < SS; ++sx) {
            double adx = std::fabs((xx + (sx + 0.5) * inv) - c);
            double ady = std::fabs((yy + (sy + 0.5) * inv) - c);
            bool vArm = adx <= outlineHalf && ady <= armLen && ady >= gap;
            bool hArm = ady <= outlineHalf && adx <= armLen && adx >= gap;
            if (vArm || hArm) {
                ++covered;
                if ((vArm && adx <= coreHalf) || (hArm && ady <= coreHalf)) ++coreCov;
            }
        }
        if (!covered) continue;                                  // fully transparent texel
        unsigned a   = (unsigned)(covered * 255 / (SS * SS));
        unsigned lum = (unsigned)(coreCov * 0xCC / covered);     // grey core vs black outline mix
        if (premultiply) lum = lum * a / 255u;                   // GDI AC_SRC_ALPHA wants premultiplied
        px[(size_t)yy * n + xx] = (a << 24) | (lum << 16) | (lum << 8) | lum;   // BGRA
    }
    return px;
}
}
