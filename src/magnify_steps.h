#pragma once
#include <cmath>
namespace wind {
// Pure step math for the magnify model (no <windows.h>): Windows Magnifier can only be driven in
// ZoomIncrement steps via injected Win+Plus/Win+Minus, so Wind's smooth ZoomController level is
// mapped to a target STEP COUNT and the model injects the delta, budgeted per tick.

// Magnifier's fullscreen ceiling is 1600%.
inline constexpr double kMagnifyMaxLevel = 16.0;

// How many increments of stepPct (percent) above 100% correspond to `level`.
// level 1.0 -> 0; level 2.0 @ 5% -> 20; clamped to [1.0, kMagnifyMaxLevel].
inline int MagnifyTargetSteps(double level, int stepPct) {
    if (stepPct <= 0) return 0;
    if (level < 1.0) level = 1.0;
    if (level > kMagnifyMaxLevel) level = kMagnifyMaxLevel;
    return (int)std::lround((level * 100.0 - 100.0) / (double)stepPct);
}

// Signed injections to perform this tick: positive = Win+Plus count, negative = Win+Minus count,
// magnitude clamped to `budget` so Magnifier is never flooded (it drops chords injected too fast).
inline int MagnifyInjectionsThisTick(int current, int target, int budget) {
    if (budget < 0) budget = 0;
    int delta = target - current;
    if (delta >  budget) return  budget;
    if (delta < -budget) return -budget;
    return delta;
}
}
