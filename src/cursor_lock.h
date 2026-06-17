#pragma once
namespace wind {
// Pure state machine for the optional "Inspect mode" cursor lock. No <windows.h> (compiles into the
// desktop-free WIND_TESTS build). The tick (main.cpp) feeds it edge events and does the Win32 freeze
// (ClipCursor) and reticle warp based on the locked() transitions; this class only owns the rules.
class CursorLockController {
public:
    // Rising edge of the bound toggle key. Ignored unless zoomed in (lock is meaningless at 1x).
    void toggle(bool zoomedIn);
    // A left/right click was observed while locked (by the mouse hook): unlock. The hook itself warps
    // the cursor to the reticle and lets the click land; this just drops the lock state.
    void commitClick();
    // Back to free. Called on zoom-out, recenter, and monitor retarget (same resets as LockDetector).
    void reset();

    bool locked() const { return locked_; }
    // While locked, pan from raw mickeys (the OS cursor is frozen, so its delta is not the pan source).
    bool panFromRaw() const { return locked_; }
private:
    bool locked_ = false;
};
}
