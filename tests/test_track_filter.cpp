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
