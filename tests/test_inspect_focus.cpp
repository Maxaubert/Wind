#include "doctest.h"
#include "../src/inspect_focus.h"

using wind::ShouldGameInspect;

TEST_CASE("zoomed while WE hide the cursor: the LockDetector decides") {
    // Render sessions hide + weld the OS cursor, so its visibility says nothing about the app.
    // Mouselook gameplay while zoomed: detector locked -> game-inspect.
    CHECK(ShouldGameInspect(true, true, true, true));
    CHECK(ShouldGameInspect(true, true, false, true));
    // Game menu / desktop while zoomed: detector free -> normal inspect, even though the
    // magnifier itself has hidden the OS cursor (cursorWasShowing is meaningless here).
    CHECK_FALSE(ShouldGameInspect(true, false, true, true));
    CHECK_FALSE(ShouldGameInspect(true, false, false, true));
}

// The bug: a transform FOLLOW session (the shipped design since issue #148) leaves the real cursor
// completely alone while zoomed - it is never hidden, frozen or welded. A raw-input game like RDR2
// therefore reads FREE on the LockDetector (the pointer still tracks the hand), so the detector-only
// rule declined game-inspect and the camera kept moving. With nothing of ours touching the cursor,
// the app's own hiding is exactly as trustworthy as it is at 1x - so use it.
TEST_CASE("zoomed while the cursor is untouched: the app hiding it is still the mouselook tell") {
    CHECK(ShouldGameInspect(true, false, false, false));       // RDR2 zoomed: the reported bug
    CHECK_FALSE(ShouldGameInspect(true, false, true, false));  // desktop zoomed: cursor visible
    CHECK(ShouldGameInspect(true, true, true, false));         // a lock still wins on its own
}

TEST_CASE("at 1x: a cursor hidden by the foreground app is the mouselook tell") {
    // Nothing of ours has hidden the cursor at 1x, so the flag is false on this path in practice;
    // pin both values anyway - the 1x rule must never consult anything but the app's own hiding.
    CHECK(ShouldGameInspect(false, false, false, false));
    CHECK(ShouldGameInspect(false, false, false, true));
    // Desktop / game menu at 1x (cursor visible) -> normal inspect.
    CHECK_FALSE(ShouldGameInspect(false, false, true, false));
    // Detector state is stale at 1x (it only updates while zoomed) and must not leak in.
    CHECK_FALSE(ShouldGameInspect(false, true, true, false));
    CHECK(ShouldGameInspect(false, true, false, false));
}
