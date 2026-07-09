// Pure decision logic for "the magnifier is gone, so close the config window".
// Deliberately free of <windows.h> so it compiles into the WIND_TESTS build.
#pragma once
namespace wind {

// running: this poll's WindRunning() result.
// armed:   caller-owned. Latches true once Wind has been observed running.
// misses:  caller-owned. Consecutive false observations since the last true.
//
// Two guards, both load-bearing:
//  - WindRunning() returns false when its process snapshot FAILS, not only when Wind is absent
//    (src/config_ui/main.cpp:35). One transient false must never close the user's window.
//  - WindConfig can legitimately show a window while Wind is down: a failed Wind.exe launch falls
//    back to onboarding (src/config_ui/main.cpp:246). Closing then would hide that very error.
inline bool ShouldCloseOnWindGone(bool running, bool& armed, int& misses) {
    if (running) { armed = true; misses = 0; return false; }
    if (!armed) return false;
    return ++misses >= 2;
}

}  // namespace wind
