#pragma once
#include <cmath>
namespace wind {
// Pure level + anchor math for the magnify model (no <windows.h>).
//
// Measured control surface of Magnify.exe (issue #146 probes, in order of discovery):
//  - It registry-watches Magnification and eases a SINGLE write beautifully (~280 ms, any span),
//    BUT writes arriving faster than that window degenerate into instant snaps at the window
//    boundary - so streaming the ramp through the registry is a dead end, as was injecting
//    Win+Plus chords (half of a rapid burst is dropped).
//  - MagSetFullscreenTransform from OUR process STICKS while Magnify.exe runs (no stomping, even
//    with mouse movement), and a registry write whose value matches the actual transform is a
//    visual no-op - a seamless handoff.
// Hence the hybrid: Wind drives the transform directly during the ramp (glass smooth, cursor-
// anchored), then hands the settled level to Magnifier via one registry write so its native
// follow-the-mouse panning owns steady state. Big single-tick jumps (quick zoom) go through the
// registry instead, buying Magnifier's eased animation for free.
//
// CAUTION: a Magnification registry value above 1600 is silently IGNORED (measured), not
// clamped - so the clamp here is mandatory. And a registry write that does NOT change the value
// fires no notification (Magnifier does nothing): never rely on a same-value write.
inline constexpr double kMagnifyMaxLevel = 16.0;

inline double MagnifyClampLevel(double level) {
    if (level < 1.0) return 1.0;
    if (level > kMagnifyMaxLevel) return kMagnifyMaxLevel;
    return level;
}

// Wind's smooth ZoomController level -> the integer percent for the registry channel.
inline int MagnifyTargetPct(double level) {
    return (int)std::lround(MagnifyClampLevel(level) * 100.0);
}

struct MagnifyOffset { double x, y; };

// Fullscreen-transform source top-left for the next ramp tick, anchored at the cursor: the
// desktop point p (cursor, desktop px) keeps its current SCREEN position s = (p - off0) * lvl0
// under the new level, so the view zooms toward/away from the cursor with no lateral jump -
// solving (p - off1) * lvl1 == s gives off1 = p - s / lvl1. Clamped to the legal source range
// [0, screen - screen/lvl1] so the view never leaves the desktop (the anchor then slides, which
// is the correct edge behavior).
inline MagnifyOffset MagnifyAnchorOffset(double px, double py, double lvl0, double ox0, double oy0,
                                         double lvl1, int screenW, int screenH) {
    if (lvl0 < 1.0) lvl0 = 1.0;
    lvl1 = MagnifyClampLevel(lvl1);
    double sx = (px - ox0) * lvl0;
    double sy = (py - oy0) * lvl0;
    double ox = px - sx / lvl1;
    double oy = py - sy / lvl1;
    double maxX = screenW - screenW / lvl1;
    double maxY = screenH - screenH / lvl1;
    if (ox < 0) ox = 0; else if (ox > maxX) ox = maxX;
    if (oy < 0) oy = 0; else if (oy > maxY) oy = maxY;
    return MagnifyOffset{ ox, oy };
}
}
