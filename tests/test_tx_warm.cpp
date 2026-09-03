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

TEST_CASE("cadence: 0 warms every tick (the pre-#246 behaviour)") {
    TxWarmIn in = Resting();
    in.mode = 1;
    in.warmHz = 0;
    in.sinceLastWarmMs = 0;
    CHECK(WarmAction(in) == TxWarm::Jitter1px);
}

TEST_CASE("cadence: a new pulse waits for its period") {
    TxWarmIn in = Resting();
    in.mode = 1;
    in.warmHz = 24;                 // period 41ms
    in.pulseOpen = false;
    in.sinceLastWarmMs = 7;         // one tick after the last pulse closed
    CHECK(WarmAction(in) == TxWarm::None);
    in.sinceLastWarmMs = 40;
    CHECK(WarmAction(in) == TxWarm::None);
    in.sinceLastWarmMs = 41;
    CHECK(WarmAction(in) == TxWarm::Jitter1px);
    in.sinceLastWarmMs = 5000;
    CHECK(WarmAction(in) == TxWarm::Jitter1px);
}

TEST_CASE("cadence: an open pulse always closes, never waits for the period") {
    // The displacing half went out last tick; the view is 1px off. The return must go out THIS
    // tick regardless of cadence, or the rest view sits displaced for a whole period.
    TxWarmIn in = Resting();
    in.mode = 1;
    in.warmHz = 12;
    in.pulseOpen = true;
    in.sinceLastWarmMs = 0;
    CHECK(WarmAction(in) == TxWarm::Jitter1px);
    // The same rule for the level-epsilon channel: both halves of its toggle must write.
    in.mode = 4;
    CHECK(WarmAction(in) == TxWarm::LevelEpsilon);
}

TEST_CASE("cadence: a real write closes an open pulse by itself") {
    // A real write this tick returns the view to the truth (it writes the true translation), so
    // no close is owed and warming stays out of its way.
    TxWarmIn in = Resting();
    in.mode = 1;
    in.warmHz = 12;
    in.pulseOpen = true;
    in.wroteThisTick = true;
    CHECK(WarmAction(in) == TxWarm::None);
}

TEST_CASE("cadence: neither the window nor the level cap can strand an open pulse") {
    TxWarmIn in = Resting(20.0);
    in.mode = 1;
    in.warmHz = 12;
    in.pulseOpen = true;
    in.windowMs = 700;
    in.sinceLastChangeMs = 100000;   // window long lapsed
    in.maxLevel = 8;                 // and the level is over the cap
    CHECK(WarmAction(in) == TxWarm::Jitter1px);
    // ...but once closed, both gates apply again.
    in.pulseOpen = false;
    CHECK(WarmAction(in) == TxWarm::None);
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
