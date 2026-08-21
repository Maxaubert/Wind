#pragma once
namespace wind {
// Is a ClipCursor rect a GAME lock signal, or merely desktop-like? (issue #169)
//
// The old test was "any clip smaller than the virtual desktop = locked", which misclassified a
// machine-wide WORK-AREA clip (desktop minus taskbar - e.g. a taskbar utility keeping the pointer
// out of it; ~95% of the monitor) as a game lock. Every zoomed desktop session then ran the locked
// path: panning from unaccelerated raw mickeys while the pointer moved with ballistics, the weld
// re-parking the pointer to the slower lens centre every tick - the #169 drag flicker, and
// drag-follow could never engage because the free branch never ran.
//
// A game confines the cursor to its WINDOW or a recenter box - meaningfully smaller than the
// monitor. A desktop-like clip spans (nearly) the whole monitor. Threshold 90%: a work-area clip
// is ~95% of the monitor's height (taskbar ~5%), a windowed game is well under. A game clipping to
// a FULL monitor never needed the clip signal - the raw-active-but-cursor-frozen detection catches
// mouselook there, same as before this rule existed.
inline bool ClipRectConfines(int clipW, int clipH, int monW, int monH) {
    if (clipW <= 0 || clipH <= 0) return true;             // degenerate (1px freeze) = confined
    if (monW  <= 0 || monH  <= 0) return false;            // no monitor info: never claim a lock
    return clipW < monW * 9 / 10 || clipH < monH * 9 / 10;
}
// Decides whether the OS cursor is "locked" by a game (so the magnifier must pan from raw mouse
// input rather than the OS cursor's own motion). Pure, with hysteresis so a single contrary tick
// never flips the state - panning never flickers. Fed per-tick Win32 signals by main.cpp.
class LockDetector {
public:
    // clipConfined: a smaller-than-virtual-desktop ClipCursor rect is active (direct lock signal).
    // rawMag    : |rawDx| + |rawDy| this tick (mouse motion at the HID level).
    // cursorMag : |cursorDx| + |cursorDy| this tick (how far the OS cursor actually moved).
    // Returns the (possibly updated) locked state.
    bool update(bool clipConfined, int rawMag, int cursorMag);
    // Warp-anchor variant (issue #221, field-traced on DOOM The Dark Ages): some mouselook
    // engines WARP the pointer back to a fixed anchor every frame instead of clipping or
    // freezing it - the trace showed 58 returns to one pixel with apparent pointer speeds of
    // 13k-80k px/s. That defeats both classic tells at once (the clip is the full monitor, and
    // the warp keeps the cursor MOVING, which the frozen-tell reads as free movement). So:
    // a big cursor jump (>= kWarpJumpPx) LANDING within a few px of the same anchor repeatedly
    // is lock evidence, and a recent warp landing suppresses the free streak (a slow game warps
    // only every few ticks, and the hand-motion ticks in between must not unlock).
    // warpTell gates the whole mechanism (the warpLock knob: engaging mid-fight reads as "the
    // magnifier hitches then gets good", so the feature is user-visible and optional).
    // cx/cy: the current OS cursor position (virtual px).
    bool update(bool clipConfined, int rawMag, int cursorMag, bool warpTell, int cx, int cy);
    bool locked() const { return locked_; }
    // True when the CURRENT locked state was reached via the warp-anchor tell (diagnostics).
    bool warpLocked() const { return locked_ && warpReturns_ > 0; }
    void reset();   // back to free (call on zoom-in / recenter / monitor retarget)
private:
    bool locked_ = false;
    int  lockStreak_ = 0;   // consecutive ticks of (raw active, OS cursor frozen)
    int  freeStreak_ = 0;   // consecutive ticks of (OS cursor moving with input)
    // Warp-anchor state (issue #221).
    bool haveAnchor_ = false;
    int  anchorX_ = 0, anchorY_ = 0;
    int  anchorMissTicks_ = 0;   // ticks since the cursor last sat on the anchor
    int  warpReturns_ = 0;       // big jumps that LANDED on the anchor
    int  sinceWarp_ = 1000;      // ticks since the last warp landing (large = long ago)
    // Confinement-box tell (issue #221, round 2): GENTLE mouselook produces warp jumps too
    // small for the anchor tell, but the signature is the same - lots of raw mickeys while the
    // cursor's positions stay inside a tiny box. Tumbling window accumulators.
    int  boxTicks_ = 0, boxRawSum_ = 0;
    int  boxMinX_ = 0, boxMaxX_ = 0, boxMinY_ = 0, boxMaxY_ = 0;
};
}
