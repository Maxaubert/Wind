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

// --- Warp-anchor tell (issue #221; DOOM The Dark Ages field trace: the game WARPS the pointer
// back to (853,480) every frame during mouselook - 58 returns in the trace - which the classic
// frozen-cursor tell reads as free movement). Constants: kAnchorTolPx=6, kWarpJumpPx=100,
// kWarpLockReturns=4, kAnchorLossTicks=45, kWarpQuietTicks=12.

namespace {
// One DOOM-style warp period: a hand-motion tick away from the anchor, then the engine warps
// the pointer back. Both ticks move the cursor a lot - that is exactly what defeats the
// classic tell.
void warpPeriod(LockDetector& d, bool warpTell, int farX = 2000, int farY = 1000) {
    d.update(false, 20, 300, warpTell, farX, farY);
    d.update(false, 20, 300, warpTell, 853, 480);
}
}

TEST_CASE("warp tell: the DOOM pattern locks after kWarpLockReturns anchor landings") {
    LockDetector d;
    d.update(false, 20, 300, true, 853, 480);   // first big landing establishes the anchor
    warpPeriod(d, true); CHECK(!d.locked());    // return 1
    warpPeriod(d, true); CHECK(!d.locked());    // return 2
    warpPeriod(d, true); CHECK(!d.locked());    // return 3
    warpPeriod(d, true);                        // return 4 -> locked
    CHECK(d.locked());
    CHECK(d.warpLocked());
}

TEST_CASE("warp tell disabled: the same DOOM pattern stays FREE (documents the #221 bug)") {
    LockDetector d;
    d.update(false, 20, 300, false, 853, 480);
    for (int i = 0; i < 20; ++i) warpPeriod(d, false);
    CHECK(!d.locked());   // every tick counts as cursor-tracking-input = free evidence
}

TEST_CASE("warp tell: anchor landings tolerate a few px of jitter") {
    LockDetector d;
    d.update(false, 20, 300, true, 853, 480);
    d.update(false, 20, 300, true, 2000, 1000);
    d.update(false, 20, 300, true, 855, 482);   // within kAnchorTolPx
    d.update(false, 20, 300, true, 2000, 1000);
    d.update(false, 20, 300, true, 850, 477);
    d.update(false, 20, 300, true, 2000, 1000);
    d.update(false, 20, 300, true, 856, 484);
    d.update(false, 20, 300, true, 2000, 1000);
    d.update(false, 20, 300, true, 853, 480);   // 4th landing
    CHECK(d.locked());
}

TEST_CASE("warp tell: a slow (30fps) warper does not flap the lock between warps") {
    LockDetector d;
    d.update(false, 20, 300, true, 853, 480);
    for (int i = 0; i < 5; ++i) warpPeriod(d, true);
    REQUIRE(d.locked());
    // 4 hand-motion ticks between warps (a 30fps game at a 144Hz tick): free evidence, but the
    // last warp is recent (< kWarpQuietTicks), so the lock must hold.
    for (int i = 0; i < 4; ++i) d.update(false, 20, 40, true, 1500 + i * 40, 900);
    CHECK(d.locked());
    warpPeriod(d, true);
    CHECK(d.locked());
}

TEST_CASE("warp tell: leaving mouselook (no more warps) unlocks after the quiet window") {
    LockDetector d;
    d.update(false, 20, 300, true, 853, 480);
    for (int i = 0; i < 5; ++i) warpPeriod(d, true);
    REQUIRE(d.locked());
    // Menu/desktop: the cursor tracks the hand, drifting, never re-landing on the anchor.
    bool locked = true;
    for (int i = 0; i < 20 && locked; ++i)
        locked = d.update(false, 10, 8, true, 1200 + i * 30, 700 + i * 10);
    CHECK(!locked);
}

TEST_CASE("warp tell: a hand flick plus resting nearby never locks (no false anchor)") {
    LockDetector d;
    d.update(false, 40, 400, true, 900, 500);   // fast flick lands here -> becomes the anchor
    for (int i = 0; i < 50; ++i)
        d.update(false, 3, 2, true, 901, 501);  // small motion near the anchor: no 100px jumps
    CHECK(!d.locked());
}

TEST_CASE("warp tell: a fast desktop pan (positions always advancing) never locks") {
    LockDetector d;
    for (int i = 0; i < 60; ++i)
        d.update(false, 30, 150, true, 100 + i * 55, 800);
    CHECK(!d.locked());
}

// Round 2 (Max: gentle mouselook needed ERRATIC motion to engage): the confinement-box tell.
// Constants: kBoxTicks=24, kBoxRawSum=400, kBoxSpanPx=30.

TEST_CASE("box tell: gentle mouselook (small jiggle, streaming mickeys) locks within a window") {
    LockDetector d;
    // ~20 mickeys/tick, cursor jiggling +/-8px around the warp point: too small for the anchor
    // tell's 100px jumps, but 480 mickeys land in a 16px box within one 24-tick window.
    for (int i = 0; i < 24; ++i)
        d.update(false, 20, 10, true, 853 + (i % 2 ? 8 : -8), 480 + (i % 3 ? 5 : -5));
    CHECK(d.locked());
    CHECK(d.warpLocked());
}

TEST_CASE("box tell: precise slow desktop work (few mickeys) never locks") {
    LockDetector d;
    // Careful nudging: ~6 mickeys/tick in a tiny area - 144/window, far under kBoxRawSum.
    for (int i = 0; i < 96; ++i)
        d.update(false, 6, 2, true, 1200 + (i % 2), 800);
    CHECK(!d.locked());
}

TEST_CASE("box tell: a sweeping pan (positions advancing) never locks") {
    LockDetector d;
    for (int i = 0; i < 96; ++i)
        d.update(false, 25, 30, true, 400 + i * 30, 900);
    CHECK(!d.locked());
}

TEST_CASE("box tell: opening the game menu (cursor roams) unlocks shortly after") {
    LockDetector d;
    for (int i = 0; i < 24; ++i)
        d.update(false, 20, 10, true, 853 + (i % 2 ? 8 : -8), 480);
    REQUIRE(d.locked());
    bool locked = true;
    for (int i = 0; i < 40 && locked; ++i)
        locked = d.update(false, 10, 12, true, 1000 + i * 25, 700 + i * 8);
    CHECK(!locked);
}

// Round 3: seeding at the zoom-in edge (app-hidden cursor over a covering foreground).

TEST_CASE("seedLock: locked immediately, and gameplay warps sustain it") {
    LockDetector d;
    d.seedLock();
    CHECK(d.locked());
    CHECK(d.warpLocked());
    for (int i = 0; i < 10; ++i) warpPeriod(d, true);
    CHECK(d.locked());
}

TEST_CASE("seedLock: a wrong seed (video player re-shows the cursor) self-heals quickly") {
    LockDetector d;
    d.seedLock();
    REQUIRE(d.locked());
    bool locked = true;
    int ticks = 0;
    // Pointer reappears and tracks the hand: free evidence, gated only by the quiet window.
    for (; ticks < 30 && locked; ++ticks)
        locked = d.update(false, 10, 8, true, 500 + ticks * 20, 600);
    CHECK(!locked);
    CHECK(ticks <= 20);   // ~kWarpQuietTicks + kFreeTicks at 144Hz = ~110ms
}

TEST_CASE("warp tell: reset clears the anchor state") {
    LockDetector d;
    d.update(false, 20, 300, true, 853, 480);
    for (int i = 0; i < 5; ++i) warpPeriod(d, true);
    REQUIRE(d.locked());
    d.reset();
    CHECK(!d.locked());
    CHECK(!d.warpLocked());
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

// issue #223: the streak/window constants were field-tuned in ticks on a 144Hz panel. At other
// refresh rates the detector re-derives them from ms baselines so real-time behavior is stable.
TEST_CASE("setTickRate keeps lock/unlock behavior in real time, not in ticks") {
    // At 144Hz the derivation reproduces the original tuned constants exactly: the classic
    // frozen-cursor lock still needs 6 raw-active ticks.
    {
        LockDetector d;
        d.setTickRate(144);
        for (int i = 0; i < 5; ++i) CHECK(!d.update(false, 20, 0));
        CHECK(d.update(false, 20, 0));   // 6th tick locks
    }
    // At 60Hz a tick is 2.4x longer, so the same ~42ms of evidence is 3 ticks.
    {
        LockDetector d;
        d.setTickRate(60);
        for (int i = 0; i < 2; ++i) CHECK(!d.update(false, 20, 0));
        CHECK(d.update(false, 20, 0));   // 3rd tick locks (~50ms at 60Hz)
    }
    // At 240Hz the same evidence takes proportionally more ticks (10 for ~42ms).
    {
        LockDetector d;
        d.setTickRate(240);
        for (int i = 0; i < 9; ++i) CHECK(!d.update(false, 20, 0));
        CHECK(d.update(false, 20, 0));
    }
    // Slow deliberate mouselook at 240Hz: per-tick mickeys halve, and the deliberate-motion
    // floor follows (4 @144 -> 2 @240) so gentle motion still counts as lock evidence.
    {
        LockDetector d;
        d.setTickRate(240);
        for (int i = 0; i < 10; ++i) d.update(false, 2, 0);
        CHECK(d.locked());
    }
    // An unconfigured detector behaves exactly as the 144Hz-tuned original (test-suite baseline).
    {
        LockDetector a, b;
        b.setTickRate(144);
        for (int i = 0; i < 6; ++i) { a.update(false, 20, 0); b.update(false, 20, 0); }
        CHECK(a.locked() == b.locked());
    }
}
