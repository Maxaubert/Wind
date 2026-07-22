#pragma once
#include <cmath>
namespace wind {
// Pure level math for the magnify model (no <windows.h>). Windows Magnifier watches the
// HKCU\Software\Microsoft\ScreenMagnifier "Magnification" registry value LIVE (measured, issue
// #146 follow-up): a bare RegSetValue is picked up within ~10 ms and eased to smoothly, with
// exact fidelity for arbitrary integer percents (137 -> 1.370). Wind therefore drives the zoom
// by writing the ramped level as a percent each time it changes by >= 1%.
//
// CAUTION: a value ABOVE Magnifier's 1600% ceiling is silently IGNORED (measured: writing 2500
// left the transform unchanged), not clamped - so the clamp below is mandatory, or a fast ramp
// past 16x would freeze the view at its last accepted level.
inline constexpr double kMagnifyMaxLevel = 16.0;

// Wind's smooth ZoomController level -> the integer percent to write. Clamped to Magnifier's
// accepted range [100, 1600].
inline int MagnifyTargetPct(double level) {
    if (level < 1.0) level = 1.0;
    if (level > kMagnifyMaxLevel) level = kMagnifyMaxLevel;
    return (int)std::lround(level * 100.0);
}
}
