#include "doctest.h"
#include "../src/foreground_state.h"
using namespace wind;
TEST_CASE("RectCoversMonitor: exact cover and overshoot are true, partial is false") {
    CHECK(RectCoversMonitor(0,0,1920,1080, 0,0,1920,1080));      // exact
    CHECK(RectCoversMonitor(-1,-1,1921,1081, 0,0,1920,1080));    // overshoot (borderless)
    CHECK_FALSE(RectCoversMonitor(0,0,1920,1000, 0,0,1920,1080));// 80px short at bottom
    CHECK_FALSE(RectCoversMonitor(100,0,1920,1080, 0,0,1920,1080));// inset left
}
