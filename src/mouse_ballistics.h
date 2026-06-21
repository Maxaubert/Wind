#pragma once
// Pure logic (NO <windows.h>): convert a raw mouse packet (HID mickeys) into the cooked pixel
// delta Windows' pointer pipeline would produce, so Inspect mode (which reads raw mickeys because
// the OS cursor is frozen and cannot be used as the speed oracle) pans at the SAME speed as the
// normal desktop cursor. Two effects are modelled:
//   1. Pointer-speed slider (Control Panel -> Mouse -> pointer speed, 1..20): an exact multiplier.
//   2. "Enhance pointer precision" acceleration: the piecewise-linear SmoothMouseX/YCurve, but
//      NORMALIZED so the low-speed gain equals the slider multiplier (slow precise movement is
//      1:1 with the desktop) and the curve only adds acceleration above that. Normalizing this way
//      cancels the absolute DPI/refresh scaling constants (whose exact values are undocumented),
//      so the match is robust without depending on those magic numbers.
// See docs/superpowers/specs for the design; issue: inspect-mode speed/DPI match.
namespace wind {

struct BallisticsConfig {
    bool   accelEnabled = false;   // "Enhance pointer precision" on (SPI_GETMOUSE third element)
    double sliderMult   = 1.0;     // pointer-speed slider 1..20 mapped to a multiplier (see PointerSpeedMultiplier)
    double inputDiv     = 6.0;     // mickeys -> curve-x divisor. Windows' nominal constant is ~3.5, but
                                   // WM_INPUT can coalesce HID reports (inflating per-packet magnitude),
                                   // which over-accelerated fast moves; a larger divisor softens the
                                   // high-speed gain to match the desktop. The slow-speed baseline is
                                   // unaffected (it normalizes to sliderMult regardless of inputDiv).
    double accelStrength = 0.3;    // 0..1 blend of the acceleration curve over the flat slider baseline.
                                   // 0 = flat (1 mickey -> sliderMult px, no acceleration); 1 = the full
                                   // normalized curve. Because WM_INPUT coalescing makes the full curve
                                   // over-accelerate, we only apply a fraction. The slow baseline is the
                                   // same at any strength (the curve only adds gain above the baseline).
    // Standard Windows 10 SmoothMouseXCurve / SmoothMouseYCurve, already divided by 65536 (the
    // registry stores them as 16.16 fixed point). Hardcoded defaults: customizing these curves is
    // rare, and the normalization above only relies on the curve SHAPE, not its absolute scale.
    double xCurve[5] = { 0.0, 0.43, 1.25, 3.86, 40.0 };       // input speed (mickeys)
    double yCurve[5] = { 0.0, 1.37, 5.30, 24.30, 568.0 };     // output (pre-normalization)
};

// Exact Windows pointer-speed slider multiplier table for slider positions 1..20 (10 = 1.0x).
// Out-of-range inputs are clamped to [1, 20].
double PointerSpeedMultiplier(int slider1to20);

// Convert one raw mouse packet (dx, dy in mickeys) to a cooked pixel delta (outX, outY), applying
// the slider multiplier and, when enabled, the normalized acceleration curve. Per-PACKET, matching
// Windows (its acceleration keys off each packet's magnitude, not elapsed time), so callers must
// feed it one WM_INPUT packet at a time and accumulate the fractional results.
void CookMickeyPacket(const BallisticsConfig& c, int dx, int dy, double& outX, double& outY);

}
