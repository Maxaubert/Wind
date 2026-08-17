#include "doctest.h"
#include "../src/tx_cadence.h"

using namespace wind;

// Defaults matching the shipped config: 60Hz cap, 2px pan granularity, 100ms settle escape.
static TxCadenceIn Base() {
    TxCadenceIn in;
    in.changed = true;
    in.applyLevel = 8.0;
    in.dMoveDest = 10;
    in.sinceLastWriteMs = 100;
    in.writeHz = 60;
    in.minOffsetPx = 2;
    in.settleMs = 100;
    return in;
}

TEST_CASE("nothing changed -> no write") {
    TxCadenceIn in = Base();
    in.changed = false;
    CHECK(ShouldWriteTransform(in) == false);
}

TEST_CASE("the 1x reset is never rate-limited (teardown must land)") {
    // Deferring this would leave the desktop magnified for an extra tick - or forever, if the
    // session ends and no further tick writes.
    TxCadenceIn in = Base();
    in.applyLevel = 1.0;
    in.sinceLastWriteMs = 0;   // would otherwise be gated by the 60Hz cap
    CHECK(ShouldWriteTransform(in) == true);
}

TEST_CASE("write rate is capped at txWriteHz (issue #204)") {
    // 60Hz -> 16ms minimum gap. Wind used to write every tick (~7ms on a 144Hz panel), which
    // measured 2x native Magnifier's rate in both regimes.
    TxCadenceIn in = Base();
    in.sinceLastWriteMs = 7;    // a 144Hz tick
    CHECK(ShouldWriteTransform(in) == false);
    in.sinceLastWriteMs = 16;   // the cap
    CHECK(ShouldWriteTransform(in) == true);
    in.sinceLastWriteMs = 40;
    CHECK(ShouldWriteTransform(in) == true);
}

TEST_CASE("writeHz=0 restores per-tick writes") {
    TxCadenceIn in = Base();
    in.writeHz = 0;
    in.sinceLastWriteMs = 1;
    CHECK(ShouldWriteTransform(in) == true);
}

TEST_CASE("sub-threshold PAN movement is coalesced, not dropped") {
    // A third of Wind's writes moved the image by exactly one pixel; native's median pan step is
    // 2.24px. Below the threshold we hold back...
    TxCadenceIn in = Base();
    in.levelMoved = false;
    in.dMoveDest = 1;
    in.sinceLastWriteMs = 20;   // past the rate cap, so only the granularity gate can apply
    CHECK(ShouldWriteTransform(in) == false);
    in.dMoveDest = 2;           // ...and at the threshold it goes out
    CHECK(ShouldWriteTransform(in) == true);
}

TEST_CASE("a held-back pan residual still lands within settleMs") {
    // THE guarantee that makes coalescing safe: without it, a 1px residual at the end of a pan
    // would never be written and the view would rest permanently off the cursor.
    TxCadenceIn in = Base();
    in.levelMoved = false;
    in.dMoveDest = 1;
    in.sinceLastWriteMs = 99;
    CHECK(ShouldWriteTransform(in) == false);
    in.sinceLastWriteMs = 100;
    CHECK(ShouldWriteTransform(in) == true);
}

TEST_CASE("a LEVEL change is never held back by the pan granularity gate") {
    // Level and geometry must stay consistent; scaling without re-centring is visible.
    TxCadenceIn in = Base();
    in.levelMoved = true;
    in.dMoveDest = 0;           // level moved but the view did not translate
    in.sinceLastWriteMs = 20;   // past the rate cap
    CHECK(ShouldWriteTransform(in) == true);
}

TEST_CASE("the final level of a stopped ramp always lands, even inside the rate cap") {
    // Otherwise a zoom settles a fraction off the level the user actually asked for.
    TxCadenceIn in = Base();
    in.levelMoved = true;
    in.rampStopped = true;
    in.sinceLastWriteMs = 1;    // deep inside the 60Hz cap
    CHECK(ShouldWriteTransform(in) == true);
}

TEST_CASE("a mid-ramp level change IS rate-limited") {
    // The ramp escape is only for the settle write; during the ramp the cap must hold or we are
    // back to 120 writes/s.
    TxCadenceIn in = Base();
    in.levelMoved = true;
    in.rampStopped = false;
    in.sinceLastWriteMs = 1;
    CHECK(ShouldWriteTransform(in) == false);
}

TEST_CASE("minOffsetPx=0 writes every change") {
    TxCadenceIn in = Base();
    in.minOffsetPx = 0;
    in.levelMoved = false;
    in.dMoveDest = 1;
    in.sinceLastWriteMs = 20;
    CHECK(ShouldWriteTransform(in) == true);
}

TEST_CASE("a fast pan is unaffected by the granularity gate") {
    // Real panning moves far more than 2px per 16ms; the gate must only catch the 1px dribble.
    TxCadenceIn in = Base();
    in.levelMoved = false;
    in.dMoveDest = 33;          // native's p95 pan step
    in.sinceLastWriteMs = 17;
    CHECK(ShouldWriteTransform(in) == true);
}

TEST_CASE("the gates cannot starve a continuous pan below the cap rate") {
    // Simulate 144Hz ticks of a steady 1px-per-tick drift (the worst case for the granularity
    // gate) and assert writes still go out at roughly the settle cadence rather than never.
    TxCadenceIn in = Base();
    in.levelMoved = false;
    unsigned long long since = 0;
    int writes = 0;
    for (int tick = 0; tick < 144; ++tick) {     // one second
        since += 7;
        in.sinceLastWriteMs = since;
        in.dMoveDest = 1;                        // never reaches the 2px threshold on its own
        if (ShouldWriteTransform(in)) { ++writes; since = 0; }
    }
    CHECK(writes >= 9);    // ~10/s via the 100ms settle escape
    CHECK(writes <= 12);
}

TEST_CASE("a normal pan lands close to native's measured rate, not 144/s") {
    // 4px per tick at 144Hz: the granularity gate passes, so the rate cap is what shapes this.
    TxCadenceIn in = Base();
    in.levelMoved = false;
    unsigned long long since = 0;
    int writes = 0;
    for (int tick = 0; tick < 144; ++tick) {
        since += 7;
        in.sinceLastWriteMs = since;
        in.dMoveDest = 4;
        if (ShouldWriteTransform(in)) { ++writes; since = 0; }
    }
    CHECK(writes >= 45);   // native panned at 48.6/s
    CHECK(writes <= 75);   // and nowhere near the old 92/s
}
