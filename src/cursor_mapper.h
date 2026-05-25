#pragma once
namespace wind {
// One frame's mapping result for the own renderer (centered cursor mode).
struct MapResult {
    double srcLeft, srcTop;              // float top-left of the source region (desktop px)
    double cursorScreenX, cursorScreenY; // where to draw the cursor sprite (screen px)
    int    clickDesktopX, clickDesktopY; // where to SetCursorPos for click hit-testing
};
// Pure centered-mode mapper. Integrates raw-input deltas into a float lens center
// (desktop px), so the world pans with sub-pixel precision while the cursor stays at
// screen center - shifting toward an edge only when the view clamps at the desktop edge.
// The click point equals the lens center: the desktop point under the drawn cursor, so a
// click through the transparent overlay lands exactly there.
class CursorMapper {
public:
    // smoothing 0..~0.95: light inertia on the lens. 0 = none (rendered center snaps to the
    // raw-accumulated target); higher = the center eases toward the target over several
    // frames, smoothing jerk and the uneven per-frame raw-delta steps (costs a little lag).
    CursorMapper(int screenW, int screenH, double sensitivity, double smoothing = 0.0);
    void reset(double centerX, double centerY);    // pin both target + rendered center
    MapResult update(int rawDx, int rawDy, double level);
    double centerX() const { return cx_; }         // rendered (smoothed) center
    double centerY() const { return cy_; }
private:
    int sw_, sh_;
    double sens_;
    double alpha_;          // per-frame easing factor (1 - smoothing), clamped
    double cx_, cy_;        // rendered center (eased)
    double tx_, ty_;        // target center (raw-accumulated)
};
}
