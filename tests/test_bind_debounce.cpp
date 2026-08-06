#include "doctest.h"
#include "../src/bind_debounce.h"

// Issue #176: the captured crawl is 8 ms of "held" every ~250 ms. At the default 25 ms
// debounce those pulses must NEVER reach the zoom, while real presses (40 ms is the
// shortest on record) pass with room.

TEST_CASE("captured phantom pulse train never passes at 25ms") {
    wind::BindDebounce d;
    unsigned long long t = 1000;
    for (int pulse = 0; pulse < 100; ++pulse) {
        // 8 ms exposure sampled at ~7 ms tick spacing, then released for 242 ms.
        CHECK_FALSE(d.update(true, t, 25));
        CHECK_FALSE(d.update(true, t + 7, 25));
        CHECK_FALSE(d.update(false, t + 8, 25));
        t += 250;
    }
}

TEST_CASE("a real hold passes once it reaches the debounce floor") {
    wind::BindDebounce d;
    CHECK_FALSE(d.update(true, 1000, 25));
    CHECK_FALSE(d.update(true, 1010, 25));
    CHECK_FALSE(d.update(true, 1024, 25));   // 24 ms: still under
    CHECK(d.update(true, 1025, 25));         // exactly 25 ms: held
    CHECK(d.update(true, 1100, 25));         // stays held
}

TEST_CASE("a 40ms tap (shortest real press on record) registers") {
    wind::BindDebounce d;
    bool held = false;
    for (unsigned long long t = 0; t <= 40; t += 7) held = d.update(true, 1000 + t, 25) || held;
    CHECK(held);
    CHECK_FALSE(d.update(false, 1041, 25));
}

TEST_CASE("release resets the clock completely") {
    wind::BindDebounce d;
    CHECK_FALSE(d.update(true, 1000, 25));
    CHECK_FALSE(d.update(false, 1010, 25));
    // A new press starts a fresh 25 ms count; the earlier 10 ms does not carry.
    CHECK_FALSE(d.update(true, 1020, 25));
    CHECK_FALSE(d.update(true, 1040, 25));
    CHECK(d.update(true, 1045, 25));
}

TEST_CASE("zero disables the debounce entirely") {
    wind::BindDebounce d;
    CHECK(d.update(true, 1000, 0));
    CHECK_FALSE(d.update(false, 1001, 0));
    CHECK(d.update(true, 1002, -5));   // negative behaves as off too
}
