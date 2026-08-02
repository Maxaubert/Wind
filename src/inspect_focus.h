#pragma once
namespace wind {
// Pure decision (no <windows.h>): should this Inspect entry engage GAME-INSPECT, i.e. treat the
// foreground app as a mouselook game that has captured the mouse? A raw-input game's camera cannot
// be blocked by any user-mode hook (the documented LL-hook limitation), so main.cpp answers "yes"
// by stealing foreground to an invisible helper window - a backgrounded game stops receiving mouse
// raw input, which is exactly why external overlay tools (Snipping Tool) work over gameplay.
//
// The tell is that a mouselook game HIDES the OS cursor. That reading is only trustworthy when
// nothing of ours has hidden it too, which is what magnifierHidCursor tracks:
//  - render sessions hide + weld the cursor for the whole zoom, so its visibility says nothing
//    about the app. There the LockDetector is the only usable signal: it already separates the two
//    regimes (a game menu with a visible free cursor reads free; mouselook's clip/recenter reads
//    locked).
//  - transform sessions (the FOLLOW design, issue #148) and plain 1x leave the cursor completely
//    alone, so the app's own hiding is directly observable and is the signal. This matters because
//    a raw-input game (RDR2) never clips or recenters the pointer, so the detector reads FREE right
//    through mouselook - detector-only declined game-inspect for exactly the games it exists for.
// A detected lock still engages on its own: it is positive evidence of capture either way.
//
// The detector is deliberately not consulted at 1x - it only updates while the overlay is active,
// so an idle Wind holds the last session's stale verdict.
inline bool ShouldGameInspect(bool zoomed, bool detectorLocked, bool cursorWasShowing,
                              bool magnifierHidCursor) {
    if (!zoomed) return !cursorWasShowing;
    if (detectorLocked) return true;
    return !magnifierHidCursor && !cursorWasShowing;
}
}
