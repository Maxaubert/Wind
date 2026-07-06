#include "transform.h"
namespace wind {
OffsetF ComputeOffsetF(double centerX, double centerY, double level, int screenW, int screenH) {
    if (level < 1.0) level = 1.0;
    double viewW = screenW / level;
    double viewH = screenH / level;
    double x = centerX - viewW / 2.0;
    double y = centerY - viewH / 2.0;
    double maxX = screenW - viewW;
    double maxY = screenH - viewH;
    if (maxX < 0) maxX = 0;
    if (maxY < 0) maxY = 0;
    if (x < 0) x = 0; else if (x > maxX) x = maxX;
    if (y < 0) y = 0; else if (y > maxY) y = maxY;
    return OffsetF{ x, y };
}

static int iround(double v) {
    int lower = (int)(v >= 0 ? v : v - 1);
    double frac = v - lower;
    if (frac < 0.5) return lower;
    if (frac > 0.5) return lower + 1;
    // frac == 0.5, banker's rounding (round half to even)
    return (lower & 1) ? lower + 1 : lower;
}

MagTransform ComputeMagTransform(double srcLeft, double srcTop, double level) {
    if (level < 1.0) level = 1.0;
    return MagTransform{
        iround(srcLeft), iround(srcTop),
        iround(-srcLeft * level), iround(-srcTop * level),
    };
}
}
