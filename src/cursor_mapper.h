#pragma once
namespace wind {
// One frame's mapping result for the own renderer (centered cursor mode).
struct MapResult {
    double srcLeft, srcTop;              // float top-left of the source region (desktop px)
    double cursorScreenX, cursorScreenY; // where to draw the cursor sprite (screen px)
    int    clickDesktopX, clickDesktopY; // where to SetCursorPos for click hit-testing
    // The un-rounded lens centre (desktop px). clickDesktop* is this rounded to a pixel (fine for
    // SetCursorPos); kept separately so sub-pixel consumers never re-derive it from the rounded pair.
    double centerX, centerY;
};
// Pure centered-mode mapper. Integrates per-tick pixel deltas into a float lens center
// (desktop px), so the world pans with sub-pixel precision while the cursor stays at
// screen center - shifting toward an edge only when the view clamps at the desktop edge.
// The click point equals the lens center: the desktop point under the drawn cursor, so a
// click through the transparent overlay lands exactly there.
class CursorMapper {
public:
    // The lens centre follows its target exactly. (The old cursorSmoothing eased it toward the
    // target over several frames: lag, never momentum, so it was removed in #430. Momentum after a
    // flick is the pan glide, src/pan_glide.h.)
    CursorMapper(int screenW, int screenH);
    void reset(double centerX, double centerY);    // pin both target + rendered center
    // Pan wall (issue #148): upper bound for the source rect's LEFT edge (desktop px), or a
    // negative value for no bound. Transform GAME sessions set 32000/level each tick: the
    // driver resets when the far-right strip is magnified above ~9.3x (field-bisected, both
    // API channels), so the lens smoothly stops short of it instead. Enforced on the center
    // clamp so lens, sprite, and click point all agree (no post-hoc snapping).
    void setMaxSourceLeft(double maxSrcX) { maxSrcX_ = maxSrcX; }
    // Y half of the pan wall (issue #191): the 16-bit wrap is PER-AXIS, and the shipped wall
    // guarded X only - the bottom strip above ~16.19x (srcY*level > 32767) was reachable-lethal.
    // Same contract as setMaxSourceLeft; self-gating (inert) at levels where 32000/level
    // exceeds the reachable srcY maximum.
    void setMaxSourceTop(double maxSrcY) { maxSrcY_ = maxSrcY; }
    // dx/dy: the pixel delta to apply to the lens center this tick (already resolved by the
    // caller - the OS cursor's own motion when free, or scaled raw input when a game locks it).
    MapResult update(int dx, int dy, double level);
    double centerX() const { return cx_; }
    double centerY() const { return cy_; }
private:
    int sw_, sh_;
    double cx_, cy_;        // lens centre (delta-accumulated, clamped)
    double maxSrcX_ = -1.0; // pan wall: max source-left (desktop px); <0 = unbounded
    double maxSrcY_ = -1.0; // pan wall, Y axis (issue #191); <0 = unbounded
};
}
