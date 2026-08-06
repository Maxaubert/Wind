#pragma once
// Pure debounce for keyboard zoom binds (issue #176).
//
// A stranded async bind key (the receiver phantom) is exposed to the polling fallback in
// ~8 ms hook-reinstall windows, ~4 times a second; each exposure advanced the zoom one tick
// from the ease-in floor - the field "zoom crawl". A bind must therefore read held
// CONTINUOUSLY for debounceMs before the zoom controller sees it: the 8 ms pulses can never
// pass, while the shortest real press on record (40 ms) clears a 25 ms floor with room.
// Only KEYBOARD zoom binds go through this; mouse side-buttons have no phantom history and
// keep their instant response. Pure logic (no windows.h) so the truth table is unit-tested.
namespace wind {

class BindDebounce {
public:
    // rawHeld: the undebounced key state this tick. nowMs: monotonic milliseconds.
    // debounceMs <= 0 disables (passes rawHeld through). Returns the debounced state.
    bool update(bool rawHeld, unsigned long long nowMs, int debounceMs) {
        if (!rawHeld) { down_ = false; return false; }
        if (debounceMs <= 0) return true;
        if (!down_) { down_ = true; downSinceMs_ = nowMs; }
        return nowMs - downSinceMs_ >= (unsigned long long)debounceMs;
    }
private:
    bool down_ = false;                     // a hold is in progress
    unsigned long long downSinceMs_ = 0;    // when it started
};

}  // namespace wind
