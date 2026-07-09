#include "doctest.h"
#include "../src/config_ui/wind_watchdog.h"

using wind::ShouldCloseOnWindGone;

TEST_CASE("never closes before Wind has been seen running") {
    bool armed = false; int misses = 0;
    CHECK_FALSE(ShouldCloseOnWindGone(false, armed, misses));
    CHECK_FALSE(ShouldCloseOnWindGone(false, armed, misses));
    CHECK_FALSE(ShouldCloseOnWindGone(false, armed, misses));
    CHECK_FALSE(armed);
}

TEST_CASE("one miss does not close (a Toolhelp snapshot failure also reports false)") {
    bool armed = false; int misses = 0;
    CHECK_FALSE(ShouldCloseOnWindGone(true, armed, misses));
    CHECK(armed);
    CHECK_FALSE(ShouldCloseOnWindGone(false, armed, misses));
}

TEST_CASE("two consecutive misses close") {
    bool armed = false; int misses = 0;
    ShouldCloseOnWindGone(true, armed, misses);
    CHECK_FALSE(ShouldCloseOnWindGone(false, armed, misses));
    CHECK(ShouldCloseOnWindGone(false, armed, misses));
}

TEST_CASE("a running observation between misses resets the counter") {
    bool armed = false; int misses = 0;
    ShouldCloseOnWindGone(true, armed, misses);
    CHECK_FALSE(ShouldCloseOnWindGone(false, armed, misses));
    CHECK_FALSE(ShouldCloseOnWindGone(true, armed, misses));
    CHECK_EQ(misses, 0);
    CHECK_FALSE(ShouldCloseOnWindGone(false, armed, misses));   // needs two again
}
