#include "doctest.h"
#include "../src/present_policy.h"
using namespace wind;

// dt for a given loop fps.
static double dtFor(double fps) { return 1.0 / fps; }

// Feed `secs` seconds of a steady `fps` loop while zoomed; returns the final choice.
static PresentChoice feed(PresentPolicy& p, double fps, double secs, int refreshHz = 144) {
    PresentChoice c = p.choice();
    const double dt = dtFor(fps);
    for (double t = 0.0; t < secs; t += dt) c = p.update(dt, /*zoomed*/true, refreshHz, false, false);
    return c;
}

TEST_CASE("starts on dcomp") {
    PresentPolicy p;
    CHECK(p.choice() == PresentChoice::Dcomp);
}

TEST_CASE("sustained low avg fps while zoomed falls back to blt") {
    PresentPolicy p;
    PresentChoice c = feed(p, 50.0, 1.3);   // ~1.3s of 50fps (< 0.7*144), past the 1s window
    CHECK(c == PresentChoice::Blt);
}

TEST_CASE("hitchy throttle (alternating fast+slow frames) still falls back to blt") {
    // The real bug case: frames alternate ~7ms and ~33ms, averaging ~50fps. The old per-frame
    // "consecutive below threshold" detector reset on every fast frame and never tripped.
    PresentPolicy p;
    PresentChoice c = PresentChoice::Dcomp;
    for (int i = 0; i < 80; ++i)   // 80 ticks, avg dt 0.02 -> ~1.6s, avg ~50fps
        c = p.update((i % 2) ? 0.033 : 0.007, /*zoomed*/true, 144, false, false);
    CHECK(c == PresentChoice::Blt);
}

TEST_CASE("full-rate zoomed does NOT fall back") {
    PresentPolicy p;
    PresentChoice c = feed(p, 144.0, 2.0);   // 2s at refresh
    CHECK(c == PresentChoice::Dcomp);
}

TEST_CASE("low fps while NOT zoomed never falls back") {
    PresentPolicy p;
    PresentChoice c = PresentChoice::Dcomp;
    for (int i = 0; i < 100; ++i) c = p.update(dtFor(20.0), /*zoomed*/false, 144, false, false);
    CHECK(c == PresentChoice::Dcomp);
}

TEST_CASE("a brief dip does not fall back") {
    // 0.3s low then full-rate recovery: the 1s window averages the dip with the good frames and
    // stays above threshold, so no switch.
    PresentPolicy p;
    feed(p, 50.0, 0.3);     // brief dip
    PresentChoice c = feed(p, 144.0, 2.0);   // recovered
    CHECK(c == PresentChoice::Dcomp);
}

TEST_CASE("fullscreen foreground cue re-probes dcomp from blt") {
    PresentPolicy p;
    feed(p, 50.0, 1.3);   // -> Blt
    REQUIRE(p.choice() == PresentChoice::Blt);
    PresentChoice c = p.update(dtFor(50.0), true, 144, /*fgFull*/true, /*fgChanged*/true);
    CHECK(c == PresentChoice::Dcomp);
    CHECK(p.lastReason() == PresentReason::Cue);
}

TEST_CASE("foreground change that is NOT fullscreen does not re-probe") {
    PresentPolicy p;
    feed(p, 50.0, 1.3);   // -> Blt
    PresentChoice c = p.update(dtFor(50.0), true, 144, /*fgFull*/false, /*fgChanged*/true);
    CHECK(c == PresentChoice::Blt);
}

TEST_CASE("backstop re-probes dcomp after ~60s on blt") {
    PresentPolicy p;
    feed(p, 50.0, 1.3);   // -> Blt
    REQUIRE(p.choice() == PresentChoice::Blt);
    bool reprobed = false;
    int ticks = 0;
    for (int i = 0; i < 4000 && !reprobed; ++i) {   // 0.02s/tick; 60s ~= 3000 ticks
        if (p.update(0.02, true, 144, false, false) == PresentChoice::Dcomp) reprobed = true;
        ++ticks;
    }
    CHECK(reprobed);
    CHECK(p.lastReason() == PresentReason::Backstop);
    CHECK(ticks >= 2900);   // did not fire early (60s / 0.02s ~= 3000 ticks)
}

TEST_CASE("reset returns to optimistic dcomp") {
    PresentPolicy p;
    feed(p, 50.0, 1.3);   // -> Blt
    p.reset();
    CHECK(p.choice() == PresentChoice::Dcomp);
}
