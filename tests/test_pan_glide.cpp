#include "doctest.h"
#include "../src/pan_glide.h"
#include <cmath>

using namespace wind;

static const double kTick = 1.0 / 144.0;

// A flick: n ticks at (dxPerTick, dyPerTick), then one still tick. Returns whether a glide started.
static bool Flick(PanGlide& g, int n, double dxPerTick, double dyPerTick, double tau, double cap = 1e9, bool ok = true) {
    double x = 1000, y = 1000;
    for (int i = 0; i < n; ++i) { x += dxPerTick; y += dyPerTick; PanGlideObserve(g, dxPerTick, dyPerTick, kTick, x, y, tau, cap, ok); }
    return PanGlideObserve(g, 0, 0, kTick, x, y, tau, cap, ok);
}

TEST_CASE("a stop after movement starts a glide in its direction; a resting hand does not (#430)") {
    PanGlide fast;
    CHECK(Flick(fast, 20, 20.0, 0.0, 0.15));          // 2880 px/s
    CHECK(fast.active);
    CHECK(fast.v0x > 2000.0);
    PanGlide normal;
    CHECK(Flick(normal, 20, 1.0, 0.0, 0.15));         // 144 px/s: an ordinary movement eases too
    PanGlide creep;
    CHECK_FALSE(Flick(creep, 20, 0.2, 0.0, 0.15));    // ~29 px/s: the hand was resting anyway
}

TEST_CASE("the cap bounds the distance at any speed and keeps the hand's start speed") {
    const double ticks[] = { 2.0, 20.0, 80.0 };
    for (double perTick : ticks) {
        PanGlide g;
        REQUIRE(Flick(g, 20, perTick, 0.0, 0.15, 10.0));   // cap 10 desktop px
        const double v0 = g.v0x;
        double x = 0, y = 0;
        while (PanGlideStep(g, kTick, x, y)) { if (!g.active) break; }
        CHECK(x - g.ox <= 10.0 + 1e-6);
        CHECK(v0 > perTick * 144.0 * 0.6);                  // started at (about) the hand's speed
    }
}

TEST_CASE("one setting: the ease time grows with the distance (#434)") {
    CHECK(PanGlideTauS(40) == doctest::Approx(0.060));   // the old separate default
    CHECK(PanGlideTauS(0) == doctest::Approx(0.040));
    CHECK(PanGlideTauS(200) == doctest::Approx(0.140));
    CHECK(PanGlideTauS(400) == doctest::Approx(0.240));
    CHECK(PanGlideTauS(1000) == doctest::Approx(0.240));
}

TEST_CASE("no glide when it is off or the session does not allow it") {
    PanGlide off;
    CHECK_FALSE(Flick(off, 20, 20.0, 0.0, 0.0));      // panGlideMs = 0
    PanGlide game;
    CHECK_FALSE(Flick(game, 20, 20.0, 0.0, 0.15, 1e9, false));
    PanGlide nocap;
    CHECK_FALSE(Flick(nocap, 20, 20.0, 0.0, 0.15, 0.0));   // panGlideMaxPx = 0
}

TEST_CASE("the glide decelerates smoothly, travels about v*tau, and ends") {
    PanGlide g;
    REQUIRE(Flick(g, 20, 20.0, 10.0, 0.15));
    double x = 0, y = 0, px = g.ox, py = g.oy, prevStep = 1e9;
    int ticks = 0;
    while (PanGlideStep(g, kTick, x, y) && ticks < 1000) {
        const double step = std::hypot(x - px, y - py);
        CHECK(step <= prevStep + 1e-9);                 // never speeds up
        CHECK((x - px) >= 0.0); CHECK((y - py) >= 0.0); // keeps its direction
        prevStep = step; px = x; py = y; ++ticks;
        if (!g.active) break;
    }
    CHECK_FALSE(g.active);
    const double travelled = std::hypot(x - g.ox, y - g.oy);
    const double expected = std::hypot(g.v0x, g.v0y) * 0.15;
    CHECK(travelled > expected * 0.9);
    CHECK(travelled <= expected + 1e-6);
}

TEST_CASE("cancel stops a glide and forgets the flick") {
    PanGlide g;
    REQUIRE(Flick(g, 20, 20.0, 0.0, 0.15));
    PanGlideCancel(g);
    CHECK_FALSE(g.active);
    double x, y;
    CHECK_FALSE(PanGlideStep(g, kTick, x, y));
    CHECK_FALSE(PanGlideObserve(g, 0, 0, kTick, 0, 0, 0.15, 1e9, true));   // no stale motion restarts it
}
