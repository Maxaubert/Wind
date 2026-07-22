#pragma once
namespace wind {
// Pure decision (no <windows.h>): should this Inspect entry engage GAME-INSPECT, i.e. treat the
// foreground app as a mouselook game that has captured the mouse? A raw-input game's camera cannot
// be blocked by any user-mode hook (the documented LL-hook limitation), so main.cpp answers "yes"
// by stealing foreground to an invisible helper window - a backgrounded game stops receiving mouse
// raw input, which is exactly why external overlay tools (Snipping Tool) work over gameplay.
//
// Signals, chosen so the working menu/desktop Inspect path is never disturbed:
//  - zoomed: the LockDetector is live and already distinguishes the two regimes (a game menu with a
//    visible free cursor reads free; mouselook's clip/recenter reads locked). The raw cursor-showing
//    flag is unusable here - the magnifier itself hides the OS cursor while zoomed.
//  - at 1x: the detector is idle, but nothing has hidden the cursor except the foreground app, so a
//    not-showing cursor at the toggle edge is the mouselook tell (menus/desktop keep it visible).
inline bool ShouldGameInspect(bool zoomed, bool detectorLocked, bool cursorWasShowing) {
    if (zoomed) return detectorLocked;
    return !cursorWasShowing;
}
}
