#include "mouse_ballistics.h"
#include <cmath>

namespace wind {

double PointerSpeedMultiplier(int s) {
    // The well-known Windows pointer-speed slider table: positions 1..20, default 10 -> 1.0x.
    static const double tbl[20] = {
        0.03125, 0.0625, 0.125, 0.25, 0.375, 0.5, 0.625, 0.75, 0.875, 1.0,
        1.25,    1.5,    1.75,  2.0,  2.25,  2.5, 2.75,  3.0,  3.25,  3.5,
    };
    if (s < 1)  s = 1;
    if (s > 20) s = 20;
    return tbl[s - 1];
}

// Piecewise-linear interpolation of yCurve over xCurve at xin. Below the first point we use the
// first segment's slope (xCurve[0] is 0); above the last point we extrapolate with the last
// segment's slope (Windows likewise extends the final segment for very fast flicks).
static double interpCurve(const double* xc, const double* yc, double xin) {
    for (int i = 0; i < 4; ++i) {
        if (xin <= xc[i + 1]) {
            double dx = xc[i + 1] - xc[i];
            if (dx <= 0.0) return yc[i];
            return yc[i] + (yc[i + 1] - yc[i]) * (xin - xc[i]) / dx;
        }
    }
    double dx = xc[4] - xc[3];
    if (dx <= 0.0) return yc[4];
    return yc[4] + (yc[4] - yc[3]) * (xin - xc[4]) / dx;
}

void CookMickeyPacket(const BallisticsConfig& c, int dx, int dy, double& outX, double& outY) {
    if (dx == 0 && dy == 0) { outX = 0.0; outY = 0.0; return; }
    const double counts = std::sqrt((double)dx * dx + (double)dy * dy);
    double gain;
    if (!c.accelEnabled || c.xCurve[1] <= 0.0 || c.yCurve[1] <= 0.0) {
        // No acceleration: a flat multiplier (1 mickey -> sliderMult px), exactly like the desktop
        // cursor with "Enhance pointer precision" off.
        gain = c.sliderMult;
    } else {
        // Normalize so the limit gain as counts->0 equals sliderMult: with the first segment slope
        // m = yCurve[1]/xCurve[1], the small-speed gain is m*(sliderMult/inputDiv)*K, so choosing
        // K = inputDiv*xCurve[1]/yCurve[1] makes that exactly sliderMult. The curve then only adds
        // acceleration above the slow baseline, and the unknown absolute DPI/refresh scale cancels.
        const double K = c.inputDiv * c.xCurve[1] / c.yCurve[1];
        const double xin = counts * c.sliderMult / c.inputDiv;
        const double yout = interpCurve(c.xCurve, c.yCurve, xin);
        const double curveGain = yout * K / counts;
        // Blend the curve's acceleration over the flat slider baseline by accelStrength (the curve
        // only ever adds gain above the baseline, so strength 0 collapses to the flat slider).
        gain = c.sliderMult + (curveGain - c.sliderMult) * c.accelStrength;
    }
    outX = dx * gain;
    outY = dy * gain;
}

}
