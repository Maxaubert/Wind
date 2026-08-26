#include "../third_party/doctest.h"
#include "../src/tx_warm.h"

using namespace wind;

// The pan-start hitch these guard is described in src/tx_warm.h with the measurements.

static TxWarmIn Resting(double level = 7.0) {
    TxWarmIn in;
    in.wroteThisTick = false;
    in.ramping = false;
    in.mode = 4;   // the tests exercise the modes; the SHIPPED default is 0 (see tx_warm.h)
    in.applyLevel = level;
    in.sinceLastChangeMs = 300;
    return in;
}

TEST_CASE("warm-keeping runs while a zoomed session rests") {
    CHECK(WarmAction(Resting()) == TxWarm::LevelEpsilon);
}

TEST_CASE("a real write this tick already woke DWM") {
    TxWarmIn in = Resting();
    in.wroteThisTick = true;
    CHECK(WarmAction(in) == TxWarm::None);
}

TEST_CASE("a ramp is already writing a new level every tick") {
    TxWarmIn in = Resting();
    in.ramping = true;
    CHECK(WarmAction(in) == TxWarm::None);
}

TEST_CASE("never warm at rest level") {
    // Poking DWM at 1x is the startup tax issue #148 removed: a live magnification path taxes
    // every cursor change any app makes, even when nothing is magnified.
    CHECK(WarmAction(Resting(1.0)) == TxWarm::None);
    CHECK(WarmAction(Resting(1.001)) == TxWarm::None);
    CHECK(WarmAction(Resting(1.01)) == TxWarm::LevelEpsilon);
}

TEST_CASE("mode 0 disables warm-keeping entirely") {
    TxWarmIn in = Resting();
    in.mode = 0;
    CHECK(WarmAction(in) == TxWarm::None);
}

TEST_CASE("modes map to their channels") {
    for (int m = 1; m <= 4; ++m) {
        TxWarmIn in = Resting();
        in.mode = m;
        TxWarm want = m == 1 ? TxWarm::Jitter1px
                    : m == 2 ? TxWarm::SameValue
                    : m == 3 ? TxWarm::InputTransform
                             : TxWarm::LevelEpsilon;
        CHECK(WarmAction(in) == want);
    }
}

TEST_CASE("level cap: 0 means no cap") {
    TxWarmIn in = Resting(20.0);
    in.maxLevel = 0;
    CHECK(WarmAction(in) == TxWarm::LevelEpsilon);
    in.maxLevel = 8;
    CHECK(WarmAction(in) == TxWarm::None);
    in.applyLevel = 7.0;
    CHECK(WarmAction(in) == TxWarm::LevelEpsilon);
}

TEST_CASE("window: 0 warms for as long as the session rests") {
    TxWarmIn in = Resting();
    in.sinceLastChangeMs = 60000;
    in.windowMs = 0;
    CHECK(WarmAction(in) == TxWarm::LevelEpsilon);

    // A bounded window stops warming after it lapses, which leaves the hitch on any pause longer
    // than the window - the reason the shipped default is unbounded.
    in.windowMs = 700;
    CHECK(WarmAction(in) == TxWarm::None);
    in.sinceLastChangeMs = 699;
    CHECK(WarmAction(in) == TxWarm::LevelEpsilon);
}
