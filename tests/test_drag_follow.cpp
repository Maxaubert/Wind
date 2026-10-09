#include "doctest.h"
#include "../src/drag_follow.h"

using wind::ShouldDragFollow;

// issue #169: the weld is suspended (FOLLOW) exactly while a button is held in a free welded
// session, and never in any regime that owns the cursor some other way.

TEST_CASE("drag-follow engages only for a button held in a free session") {
    CHECK(ShouldDragFollow(false, false, true));         // the one true case
    CHECK_FALSE(ShouldDragFollow(false, false, false));  // no button -> weld as normal
}

TEST_CASE("regimes that own the cursor veto drag-follow regardless of buttons") {
    // locked: a game clips/recenters the pointer; raw mickeys pan. A held button is normal there.
    CHECK_FALSE(ShouldDragFollow(true, false, true));
    // inspect: pointer frozen at the anchor; clicks are routed to the look point.
    CHECK_FALSE(ShouldDragFollow(false, true, true));
    // combinations stay vetoed.
    CHECK_FALSE(ShouldDragFollow(true, true, true));
}
