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
    bool locked() const { return locked_; }
    void reset();   // back to free (call on zoom-in / recenter / monitor retarget)
private:
    bool locked_ = false;
    int  lockStreak_ = 0;   // consecutive ticks of (raw active, OS cursor frozen)
    int  freeStreak_ = 0;   // consecutive ticks of (OS cursor moving with input)
};
}
