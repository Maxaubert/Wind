#pragma once
// Event-driven idle (issue #71): may the main loop sleep until an input event or the ~100 ms
// housekeeping timeout, instead of ticking at the monitor refresh? Pure; tests/test_idle_policy.cpp.
// Evaluated LIVE right before the wait (not cached by the previous tick), so a request that arrived
// during the tick (a quick-zoom hotkey, a press) is never slept on.
namespace wind {
struct IdleInputs {
    bool active = false;            // zoomed or Inspect: the paced paths own the loop
    bool anyHold = false;           // a zoom direction held (button/click binds or bound keys)
    bool wheelPending = false;      // wheel steps not yet drained
    bool quickZoomPending = false;  // a hotkey-mode quick zoom waiting for RunTick
    bool settling = false;          // restAfterReveal / revealPending / zoom glide / pan motion
    double msSinceActive = 1e9;     // since the last active tick (teardown, rests, tint re-apply)
    bool mouseHook = false;         // the WH_MOUSE_LL hook is installed (else buttons are polled)
    bool keyboardPolled = false;    // a keyboard bind is only visible to GetAsyncKeyState polling
    bool wakeHandle = false;        // the wake event exists (a null handle would make the wait fail)
};
inline constexpr double kIdleSettleMs = 500.0;   // full-rate ticks after a zoom session ends
inline constexpr unsigned kIdleTimeoutMs = 100;  // housekeeping cadence while asleep

inline bool IdleSleepOk(const IdleInputs& in) {
    if (in.active || in.anyHold || in.wheelPending || in.quickZoomPending || in.settling) return false;
    if (in.msSinceActive < kIdleSettleMs) return false;
    if (!in.mouseHook || in.keyboardPolled || !in.wakeHandle) return false;
    return true;
}
}  // namespace wind
