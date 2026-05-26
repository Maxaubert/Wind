#pragma once
namespace wind {
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
