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

OffsetF ComputeFixedPointOffset(double centerX, double centerY, double level) {
    if (level < 1.0) level = 1.0;
    // T(p) = (p - off) * level. Solving T(center) == center gives off = center * (1 - 1/level).
    const double k = 1.0 - 1.0 / level;
    return OffsetF{ centerX * k, centerY * k };
}

static int iround(double v) {
    int lower = (int)(v >= 0 ? v : v - 1);
    double frac = v - lower;
    if (frac < 0.5) return lower;
    if (frac > 0.5) return lower + 1;
    // frac == 0.5, banker's rounding (round half to even)
    return (lower & 1) ? lower + 1 : lower;
}

MagTransform ComputeMagTransform(double srcLeft, double srcTop, double level,
                                 int screenW, int screenH) {
    if (level < 1.0) level = 1.0;
    int offX = iround(srcLeft), offY = iround(srcTop);
    int txX = iround(-srcLeft * level), txY = iround(-srcTop * level);
    // Bound the source rect STRICTLY INSIDE the desktop (issue #148 trigger 2, see the header
    // note) with a 2px safety margin. The margin matters: at the EXACT level cap (e.g. 12.0 on
    // 3840: maxX = 3520 whole), a bare floor still lets the source rect END exactly at the
    // texture edge, and the driver's scaler reading its filter neighborhood there walks off the
    // texture - field-confirmed TDR that hit ONLY at max zoom in the right/bottom corner (the
    // same corner passed at fractional mid-ramp levels, which the floor alone keeps inside).
    // Public offsets: off + screenW/level <= screenW - margin.
    // Private translations: the same bound in level-space, margin scaled by level.
    const double kMargin = 2.0;
    const double maxX = screenW - screenW / level - kMargin;
    const double maxY = screenH - screenH / level - kMargin;
    if (offX > (int)maxX) offX = (int)maxX;
    if (offY > (int)maxY) offY = (int)maxY;
    if (offX < 0) offX = 0;
    if (offY < 0) offY = 0;
    const double minTx = -((double)screenW * (level - 1.0)) + kMargin * level;
    const double minTy = -((double)screenH * (level - 1.0)) + kMargin * level;
    if (txX < (int)minTx) txX = (int)minTx;
    if (txY < (int)minTy) txY = (int)minTy;
    if (txX > 0) txX = 0;
    if (txY > 0) txY = 0;
    return MagTransform{ offX, offY, txX, txY };
}

InputTransformRects ComputeInputTransformRects(double srcLeft, double srcTop, double level,
                                               int monX, int monY, int monW, int monH) {
    if (level < 1.0) level = 1.0;
    InputTransformRects r{};
    r.sl = monX + iround(srcLeft);
    r.st = monY + iround(srcTop);
    r.sr = monX + iround(srcLeft + monW / level);
    r.sb = monY + iround(srcTop + monH / level);
    r.dl = monX; r.dt = monY;
    r.dr = monX + monW; r.db = monY + monH;
    return r;
}
}
