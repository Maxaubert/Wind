#include "doctest.h"
#include "../src/lock_detector.h"

using wind::LockDetector;

// Mirrors the constants in lock_detector.cpp: kLockTicks=6, kFreeTicks=3, kRawActive=4, kCursorMoved=1.

TEST_CASE("starts free") {
    LockDetector d;
    CHECK(!d.locked());
}

TEST_CASE("a confined clip rect locks immediately") {
    LockDetector d;
    CHECK(d.update(/*clipConfined=*/true, 0, 0));
    CHECK(d.locked());
}

TEST_CASE("raw active + cursor frozen for kLockTicks locks (hysteresis: not before)") {
    LockDetector d;
    for (int i = 0; i < 5; ++i) CHECK(!d.update(false, 10, 0));   // 5 < kLockTicks
    CHECK(d.update(false, 10, 0));                                // 6th -> locked
    CHECK(d.locked());
}

TEST_CASE("a single frozen tick does not lock") {
    LockDetector d;
    d.update(false, 10, 0);
    CHECK(!d.locked());
}

TEST_CASE("once locked, cursor tracking input for kFreeTicks unlocks (not before)") {
    LockDetector d;
    for (int i = 0; i < 6; ++i) d.update(false, 10, 0);   // -> locked
    REQUIRE(d.locked());
    CHECK(d.update(false, 10, 5));   // moving, streak 1
    CHECK(d.update(false, 10, 5));   // streak 2 -> still locked
    CHECK(d.locked());
    CHECK(!d.update(false, 10, 5));  // streak 3 == kFreeTicks -> free
    CHECK(!d.locked());
}

TEST_CASE("idle ticks hold the current state") {
    LockDetector d;
    for (int i = 0; i < 6; ++i) d.update(false, 10, 0);   // locked
    REQUIRE(d.locked());
    d.update(false, 0, 0);           // idle: no raw, no cursor move
    CHECK(d.locked());               // still locked
}

TEST_CASE("slow desktop motion (cursor moves occasionally) never locks") {
    LockDetector d;
    // raw active every tick, but the cursor moves >=1px every other tick (accel sub-pixel
    // accumulating) - the moving ticks reset the lock streak, so it never reaches kLockTicks.
    for (int i = 0; i < 30; ++i) {
        int curMag = (i % 2 == 0) ? 0 : 2;
        d.update(false, 6, curMag);
        CHECK(!d.locked());
    }
}

TEST_CASE("reset returns to free") {
    LockDetector d;
    d.update(true, 0, 0);
    REQUIRE(d.locked());
    d.reset();
    CHECK(!d.locked());
}

// issue #169: a clip rect is a lock signal only when meaningfully smaller than the monitor. A
// machine-wide work-area clip (desktop minus taskbar) misclassified as a game lock put every
// zoomed desktop session on the locked path - the window-drag flicker.
TEST_CASE("ClipRectConfines separates game clips from desktop-like clips") {
    // Full monitor (unclipped GetCursorClip on a single monitor): free.
    CHECK(!wind::ClipRectConfines(3840, 2160, 3840, 2160));
    // Work-area clip: full width, ~95% height (taskbar shaved off): still desktop-like.
    CHECK(!wind::ClipRectConfines(3840, 2052, 3840, 2160));
    // Multi-monitor virtual desktop clip is LARGER than one monitor: free.
    CHECK(!wind::ClipRectConfines(7680, 2160, 3840, 2160));
    // A windowed game clipping to its window: confined.
    CHECK(wind::ClipRectConfines(1920, 1080, 3840, 2160));
    // Narrow-only or short-only clips still confine (either dimension under 90%).
    CHECK(wind::ClipRectConfines(3000, 2160, 3840, 2160));
    CHECK(wind::ClipRectConfines(3840, 1700, 3840, 2160));
    // The 1px freeze clip: confined.
    CHECK(wind::ClipRectConfines(1, 1, 3840, 2160));
    // Degenerate/empty clip rect: confined; missing monitor info: never claim a lock.
    CHECK(wind::ClipRectConfines(0, 0, 3840, 2160));
    CHECK(!wind::ClipRectConfines(1920, 1080, 0, 0));
}
