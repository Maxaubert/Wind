#pragma once
// Free-cursor source-rect geometry (issue #206) - PURE, no <windows.h>, so it is unit-testable.
//
// This is the formula native Magnifier uses, measured off the real thing with
// tools/mag_formula_probe.ps1 (92/99 exact) and tools/mag_trackmode_probe.ps1 (45/45 twelve-pixel
// steps tracked 1:1):
//
//     offset = clamp(cursor - screen/(2*level), 0, screen - screen/level)
//
// It lives here, in one place, because from #206 stage 2 there are TWO callers: the mouse hook
// (which writes inline, for latency) and the tick thread (level ramps, and the case where nothing
// is moving). If those two ever computed the source rect even slightly differently, they would
// fight each other every time both ran - writing alternating positions at tick rate, which is
// precisely the wobble class #205 just removed. One formula, one implementation, no divergence.
namespace wind {

struct FreeCursorSrc { double left, top; };

// cursorLocal: cursor position in LOCAL monitor pixels (virtual coords minus the monitor origin).
// maxSrcX/maxSrcY: the MPO pan wall (issue #148/#191), or negative for unbounded.
inline FreeCursorSrc ComputeFreeCursorSrc(double cursorLocalX, double cursorLocalY,
                                          double level, int monW, int monH,
                                          double maxSrcX, double maxSrcY) {
    FreeCursorSrc r{ 0.0, 0.0 };
    if (level <= 1.0 || monW <= 0 || monH <= 0) return r;
    const double viewW = monW / level;
    const double viewH = monH / level;
    r.left = cursorLocalX - viewW * 0.5;
    r.top  = cursorLocalY - viewH * 0.5;
    // Upper bound first, then the floor: on a degenerate level the two can cross, and clamping to 0
    // last guarantees a non-negative source rect. Sampling outside the desktop texture is the
    // issue #148 driver-reset class, so this order is load-bearing rather than stylistic.
    const double maxL = monW - viewW;
    const double maxT = monH - viewH;
    if (r.left > maxL) r.left = maxL;
    if (r.top  > maxT) r.top  = maxT;
    // MPO pan wall: |src*level| must stay inside the driver's 16-bit field.
    if (maxSrcX >= 0.0 && r.left > maxSrcX) r.left = maxSrcX;
    if (maxSrcY >= 0.0 && r.top  > maxSrcY) r.top  = maxSrcY;
    if (r.left < 0.0) r.left = 0.0;
    if (r.top  < 0.0) r.top  = 0.0;
    return r;
}

}  // namespace wind
