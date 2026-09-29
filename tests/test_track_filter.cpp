#include "doctest.h"
#include "../src/track_filter.h"
using namespace wind;

TEST_CASE("caret inside its element is trusted; the field-recorded Firefox iframe caret is not") {
    const TrackBox input{ 2226, 997, 3024, 1071 };
    CHECK(CaretInsideElement({ 2453, 991, 2478, 1077 }, input));            // the good reading
    CHECK_FALSE(CaretInsideElement({ 3097, 1226, 3098, 1344 }, input));     // below and right of it
    CHECK(CaretInsideElement({ 3020, 1000, 3022, 1068 }, input));           // at the right end, inside
    CHECK(CaretInsideElement({ 5, 5, 6, 20 }, { 0, 0, 0, 0 }));             // unknown element: trust
}
TEST_CASE("terminal caret beside its cell stays trusted") {
    CHECK(CaretInsideElement({ 332, 1692, 343, 1737 }, { 322, 1692, 346, 1737 }));
}
TEST_CASE("page-sized focus is a container; controls and huge zoomed inputs partly off-screen are not") {
    CHECK(IsContainerFocus({ 422, 122, 3822, 2034 }, 0, 0, 3840, 2160));      // the document
    CHECK_FALSE(IsContainerFocus({ 2226, 997, 3024, 1071 }, 0, 0, 3840, 2160));
    CHECK_FALSE(IsContainerFocus({ 902, 912, 6146, 2106 }, 0, 0, 3840, 2160)); // 5244x1194, 41% on screen
    CHECK_FALSE(IsContainerFocus({ 5000, 0, 6000, 100 }, 0, 0, 3840, 2160));  // off the monitor
}

// --- caret jumps no key explains (issue #293) -------------------------------------------
TEST_CASE("the field case: typing in Claude Code, the terminal caret leaps to a row far above") {
    CaretJumpGate g;
    CaretGateBaseline(g, { 53, 1737, 77, 1782 });
    CHECK(CaretGateStep(g, { 98, 1737, 122, 1782 }, false));     // typing on the input line: followed
    CHECK_FALSE(CaretGateStep(g, { 121, 567, 123, 612 }, false)); // space, then y=567: held
    CHECK(g.pending);
    CHECK(CaretGateStep(g, { 143, 1737, 167, 1782 }, false));    // back on the input line: followed
    CHECK_FALSE(g.pending);
}
TEST_CASE("a held jump is followed once typing continues right next to it") {
    CaretJumpGate g;
    CaretGateBaseline(g, { 100, 1700, 102, 1745 });
    CHECK_FALSE(CaretGateStep(g, { 100, 300, 102, 345 }, false));
    CHECK(CaretGateStep(g, { 122, 300, 124, 345 }, false));      // one character further on that row
    CHECK_FALSE(g.pending);
    CHECK(CaretGateStep(g, { 144, 300, 146, 345 }, false));      // and it is the new reference
}
TEST_CASE("jumps a key explains are followed at once") {
    CaretJumpGate g;
    CaretGateBaseline(g, { 100, 1700, 102, 1745 });
    CHECK(CaretGateStep(g, { 100, 300, 102, 345 }, true));       // Ctrl+Home, Page Up, ...
    CHECK(IsJumpKey(0x21, false)); CHECK(IsJumpKey(0x28, false)); CHECK(IsJumpKey(0x0D, false));
    CHECK(IsJumpKey(0x09, false)); CHECK(IsJumpKey(0x72, false)); CHECK(IsJumpKey('Z', true));
    CHECK_FALSE(IsJumpKey(0x20, false));                          // space
    CHECK_FALSE(IsJumpKey('A', false)); CHECK_FALSE(IsJumpKey(0x08, false));
}
TEST_CASE("small moves are never held: new line, wrapped typing") {
    CaretJumpGate g;
    CaretGateBaseline(g, { 100, 1000, 102, 1045 });
    CHECK(CaretGateStep(g, { 10, 1045, 12, 1090 }, false));      // wrapped to the next line
    CHECK(CaretGateStep(g, { 10, 1180, 12, 1225 }, false));      // three lines down: still near
    CHECK_FALSE(IsFarCaretJump({ 0, 0, 2, 45 }, { 0, 135, 2, 180 }));
    CHECK(IsFarCaretJump({ 0, 0, 2, 45 }, { 0, 136, 2, 181 }));
}
TEST_CASE("without a reference nothing is held") {
    CaretJumpGate g;
    CHECK(CaretGateStep(g, { 0, 2000, 2, 2045 }, false));
}
