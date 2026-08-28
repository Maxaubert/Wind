#include "../third_party/doctest.h"
#include "../src/gain_learner.h"
#include <cmath>

using namespace wind;

// The learner exists so locked-regime panning moves at EXACTLY the user's desktop cursor speed,
// measured from Windows' own in/out ratio rather than modelled - see gain_learner.h.

TEST_CASE("unlearned state is raw passthrough") {
    GainLearner g;
    CHECK(g.gainFor(10, 7.0) == doctest::Approx(1.0));
    CHECK(!g.warmedUp());
}

TEST_CASE("a learned ratio is replayed at the same speed") {
    GainLearner g;
    // Windows moved the cursor 2.5px per mickey at ~1.4 mickeys/ms (slider+curve combined).
    for (int i = 0; i < 100; ++i) g.observe(10.0, 25.0, 7.0);
    // Interpolation trades exact replay for continuity: a query off the bin centre
    // blends toward the neighbour, so a few percent of drift is by design.
    CHECK(g.gainFor(10.0, 7.0) == doctest::Approx(2.5).epsilon(0.06));
    CHECK(g.warmedUp() == false);   // one bin is not enough to call it warmed
    for (int i = 0; i < 100; ++i) g.observe(100.0, 400.0, 7.0);   // faster speed, higher gain
    CHECK(g.warmedUp());
    CHECK(g.gainFor(100.0, 7.0) == doctest::Approx(4.0).epsilon(0.06));
}

TEST_CASE("different speeds learn different gains - the acceleration curve") {
    GainLearner g;
    for (int i = 0; i < 200; ++i) {
        g.observe(5.0,   5.0, 7.0);    // slow: 1.0 (the 1:1 baseline)
        g.observe(150.0, 600.0, 7.0);  // flick: 4.0
    }
    CHECK(g.gainFor(5.0, 7.0)   == doctest::Approx(1.0).epsilon(0.15));  // pulled toward the fast bin
    CHECK(g.gainFor(150.0, 7.0) == doctest::Approx(4.0).epsilon(0.06));
}

TEST_CASE("an unlearned bin borrows from the nearest learned one") {
    GainLearner g;
    for (int i = 0; i < 100; ++i) g.observe(10.0, 20.0, 7.0);
    // A much faster, never-observed speed still gets the learned 2.0 rather than a cold 1.0.
    CHECK(g.gainFor(300.0, 7.0) == doctest::Approx(2.0).epsilon(0.01));
}

TEST_CASE("garbage samples are rejected") {
    GainLearner g;
    g.observe(1.0, 100.0, 7.0);     // below kMinCounts: quantization garbage
    g.observe(50.0, 0.0, 7.0);      // zero out: cursor clamped at an edge
    g.observe(50.0, 5000.0, 7.0);   // gain 100: impossible, glitch
    g.observe(50.0, 100.0, 0.0);    // no timing
    CHECK(g.gainFor(50.0, 7.0) == doctest::Approx(1.0));
}

TEST_CASE("a settings change re-converges instead of averaging forever") {
    GainLearner g;
    for (int i = 0; i < 300; ++i) g.observe(10.0, 10.0, 7.0);    // slider 1.0 era
    CHECK(g.gainFor(10.0, 7.0) == doctest::Approx(1.0).epsilon(0.06));
    for (int i = 0; i < 300; ++i) g.observe(10.0, 20.0, 7.0);    // user doubles the slider
    CHECK(g.gainFor(10.0, 7.0) == doctest::Approx(2.0).epsilon(0.06));
}

TEST_CASE("coalescing invariance: the same hand speed learns the same gain regardless of packet grouping") {
    // The property every modelled approach failed: 10 mickeys over 7ms and 20 over 14ms are the
    // SAME hand speed, and must bin identically.
    GainLearner a, b;
    for (int i = 0; i < 100; ++i) a.observe(10.0, 25.0, 7.0);
    for (int i = 0; i < 100; ++i) b.observe(20.0, 50.0, 14.0);
    CHECK(a.gainFor(10.0, 7.0) == doctest::Approx(b.gainFor(20.0, 14.0)).epsilon(0.001));
}

TEST_CASE("the learned curve round-trips through text") {
    GainLearner g;
    for (int i = 0; i < 100; ++i) { g.observe(10.0, 25.0, 7.0); g.observe(150.0, 600.0, 7.0); }
    char buf[1024];
    g.serialize(buf, sizeof(buf));
    GainLearner h;
    CHECK(h.deserialize(buf));
    CHECK(h.gainFor(10.0, 7.0)  == doctest::Approx(g.gainFor(10.0, 7.0)));
    CHECK(h.gainFor(150.0, 7.0) == doctest::Approx(g.gainFor(150.0, 7.0)));
    CHECK(h.warmedUp());
}

TEST_CASE("a corrupt persistence file is rejected whole, leaving a fresh learner") {
    GainLearner g;
    CHECK(!g.deserialize("garbage"));
    CHECK(!g.deserialize("2.0 50\n999.0 50\n"));       // impossible gain mid-file
    CHECK(!g.deserialize(nullptr));
    CHECK(g.gainFor(10.0, 7.0) == doctest::Approx(1.0));   // still pristine
}

TEST_CASE("gain is interpolated between bins, not stepped") {
    // Field report: pan speed jumped as the hand crossed a bin boundary - stepping. The replayed
    // curve must be continuous: a rate between two learned bins gets a value BETWEEN their gains.
    GainLearner g;
    for (int i = 0; i < 200; ++i) {
        g.observe(5.0,   5.0, 7.0);      // slow bin: gain 1.0
        g.observe(150.0, 600.0, 7.0);    // fast bin: gain 4.0
    }
    // A mid rate must land strictly between, and grow monotonically with rate.
    const double lo  = g.gainFor(5.0, 7.0);
    const double mid = g.gainFor(30.0, 7.0);
    const double hi  = g.gainFor(150.0, 7.0);
    CHECK(lo < mid);
    CHECK(mid < hi);
    // And nearby rates must give nearby gains (no cliff at a bin edge).
    const double a = g.gainFor(28.0, 7.0);
    const double b = g.gainFor(32.0, 7.0);
    CHECK(std::abs(a - b) < 0.5);
}
