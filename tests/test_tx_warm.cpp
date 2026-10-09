#include "../third_party/doctest.h"
#include "../src/tx_warm.h"

using namespace wind;

// The pan-start hitch these guard is described in src/tx_warm.h with the measurements.

static TxWarmIn Resting(double level = 7.0) {
    TxWarmIn in;
    in.wroteThisTick = false;
    in.ramping = false;
    in.mode = 1;   // the shipped mode (0 = off); the retired modes 2-4 read as 1 in ParseConfig
    in.applyLevel = level;
    return in;
}

TEST_CASE("warm-keeping runs while a zoomed session rests") {
    CHECK(WarmAction(Resting()) == TxWarm::Jitter1px);
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
    CHECK(WarmAction(Resting(1.01)) == TxWarm::Jitter1px);
}

TEST_CASE("mode 0 disables warm-keeping entirely") {
    TxWarmIn in = Resting();
    in.mode = 0;
    CHECK(WarmAction(in) == TxWarm::None);
}

TEST_CASE("warming has no level cap and no time window: it runs for as long as the session rests") {
    TxWarmIn in = Resting(20.0);
    CHECK(WarmAction(in) == TxWarm::Jitter1px);
}

TEST_CASE("cadence: 0 warms every tick (the pre-#246 behaviour)") {
    TxWarmIn in = Resting();
    in.warmHz = 0;
    in.sinceLastWarmMs = 0;
    CHECK(WarmAction(in) == TxWarm::Jitter1px);
}

TEST_CASE("cadence: a new pulse waits for its period") {
    TxWarmIn in = Resting();
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
    in.warmHz = 12;
    in.pulseOpen = true;
    in.sinceLastWarmMs = 0;
    CHECK(WarmAction(in) == TxWarm::Jitter1px);
}

TEST_CASE("cadence: a real write closes an open pulse by itself") {
    // A real write this tick returns the view to the truth (it writes the true translation), so
    // no close is owed and warming stays out of its way.
    TxWarmIn in = Resting();
    in.warmHz = 12;
    in.pulseOpen = true;
    in.wroteThisTick = true;
    CHECK(WarmAction(in) == TxWarm::None);
}

TEST_CASE("no pulses for a free pointer's view, but an open pulse still closes") {
    TxWarmIn in = Resting();
    in.allowed = false;                       // following a caret with a free pointer
    CHECK(WarmAction(in) == TxWarm::None);
    in.pulseOpen = true;                      // owner changed mid-pulse: the return half still goes out
    CHECK(WarmAction(in) == TxWarm::Jitter1px);
    in = Resting();
    in.allowed = true;                        // locked mouselook or Inspect
    CHECK(WarmAction(in) == TxWarm::Jitter1px);
}
