#include "doctest.h"
#include "../src/inspect_focus.h"

using wind::ShouldGameInspect;

TEST_CASE("zoomed: the LockDetector decides, cursor visibility is ignored") {
    // Mouselook gameplay while zoomed: detector locked -> game-inspect.
    CHECK(ShouldGameInspect(true, true, true));
    CHECK(ShouldGameInspect(true, true, false));
    // Game menu / desktop while zoomed: detector free -> normal inspect, even though the
    // magnifier itself has hidden the OS cursor (cursorWasShowing is meaningless here).
    CHECK_FALSE(ShouldGameInspect(true, false, true));
    CHECK_FALSE(ShouldGameInspect(true, false, false));
}

TEST_CASE("at 1x: a cursor hidden by the foreground app is the mouselook tell") {
    // Gameplay at 1x (game hid the cursor for mouselook) -> game-inspect.
    CHECK(ShouldGameInspect(false, false, false));
    // Desktop / game menu at 1x (cursor visible) -> normal inspect.
    CHECK_FALSE(ShouldGameInspect(false, false, true));
    // Detector state is stale at 1x (it only updates while zoomed) and must not leak in.
    CHECK_FALSE(ShouldGameInspect(false, true, true));
    CHECK(ShouldGameInspect(false, true, false));
}
