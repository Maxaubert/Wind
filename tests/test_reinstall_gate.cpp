#include "doctest.h"
#include "../src/reinstall_gate.h"

// Issue #176: the watchdog fired a reinstall every 250 ms for 7 minutes. The gate caps
// that to 1 per 2 s, 3 per 30 s window, then a 30 s cooldown.

TEST_CASE("first request passes immediately (real evictions heal fast)") {
    wind::ReinstallGate g;
    CHECK(g.allow(5000));
}

TEST_CASE("250ms storm is throttled to the spec cadence") {
    wind::ReinstallGate g;
    int allowed = 0;
    unsigned long long firstDenied = 0;
    for (unsigned long long t = 0; t <= 120000; t += 250) {
        if (g.allow(t)) ++allowed;
    }
    // 0, 2000, 4000 allowed (3 in the window); cooldown until 34000; then a fresh window:
    // 34000, 36000, 38000; cooldown until 68000; then 68000, 70000, 72000; cooldown until
    // 102000; then 102000, 104000, 106000. 12 total in 120 s - versus 480 unthrottled.
    CHECK(allowed == 12);
    (void)firstDenied;
}

TEST_CASE("min gap of 2s between allowed requests") {
    wind::ReinstallGate g;
    CHECK(g.allow(1000));
    CHECK_FALSE(g.allow(1500));
    CHECK_FALSE(g.allow(2999));
    CHECK(g.allow(3000));
}

TEST_CASE("sparse real evictions are never throttled") {
    wind::ReinstallGate g;
    // One eviction per minute, forever: every single one passes.
    for (unsigned long long t = 0; t < 600000; t += 60000) CHECK(g.allow(t));
}

TEST_CASE("cooldown ends and the gate reopens") {
    wind::ReinstallGate g;
    CHECK(g.allow(0));
    CHECK(g.allow(2000));
    CHECK(g.allow(4000));
    CHECK_FALSE(g.allow(6000));    // 4th in the window: trips the cooldown (until 34000)
    CHECK_FALSE(g.allow(33999));
    CHECK(g.allow(34000));
}
