#include "doctest.h"
#include "../src/raw_rate_window.h"

using namespace wind;

// Units: 1 tick = 1 "microsecond"; a 144Hz tick interval is 6944 of them.
static const long long kWin = 6944;

TEST_CASE("one tick aggregate over a full window is that aggregate") {
    RawRateWindow w;
    w.push(0, kWin, 7.0);
    CHECK(w.countsIn(kWin, kWin) == doctest::Approx(7.0));
}

TEST_CASE("a steady packet stream measures the same rate as its tick aggregate") {
    // 430 packets/s of 2 counts = 2325us apart; the aggregate over one tick is 5.97 counts.
    RawRateWindow packets, ticks;
    const long long period = 2325;
    double lastAggregate = 0.0, lastPacket = 0.0;
    long long prevDrain = 0;
    for (long long t = period; t <= 40 * kWin; t += period) {
        packets.push(prevDrain, t, 2.0);
        lastPacket = packets.countsIn(t, kWin);
        prevDrain = t;
    }
    // The tick path: one drain per tick with everything that arrived in it.
    long long prevTick = 0; int n = 0;
    for (long long t = kWin; t <= 40 * kWin; t += kWin) {
        // packets in (prevTick, t]
        const long long first = prevTick / period + 1, last = t / period;
        ticks.push(prevTick, t, 2.0 * double(last - first + 1));
        lastAggregate = ticks.countsIn(t, kWin);
        prevTick = t; ++n;
    }
    const double expected = 2.0 * double(kWin) / double(period);   // 5.97
    CHECK(lastPacket == doctest::Approx(expected).epsilon(0.02));
    CHECK(lastAggregate == doctest::Approx(expected).epsilon(0.02));
}

TEST_CASE("packets queued during the tick do not read as a burst") {
    // Three packets that arrived while the tick ran are drained together 0.5ms after the tick's
    // own drain: a per-drain rate would say 6 counts per 0.5ms; the window says 6 per tick.
    RawRateWindow w;
    w.push(0, kWin, 6.0);              // the tick's aggregate
    w.push(kWin, kWin + 500, 6.0);     // the queued packets, drained as one
    const double c = w.countsIn(kWin + 500, kWin);
    // Window (500, 7444]: 6444/6944 of the tick's sample + all of the second.
    CHECK(c == doctest::Approx(6.0 * 6444.0 / 6944.0 + 6.0).epsilon(0.001));
}

TEST_CASE("a pause empties the window") {
    RawRateWindow w;
    w.push(0, kWin, 7.0);
    CHECK(w.countsIn(kWin * 3, kWin) == doctest::Approx(0.0));
    CHECK(w.size() == 0);
}

TEST_CASE("the ring stays bounded under a fast stream") {
    RawRateWindow w;
    // An 8kHz mouse coalesced to the sub-tick's 0.8ms floor is ~9 drains per window; 200us
    // drains (35 per window) leave headroom under the cap and still must read exactly.
    for (long long t = 200; t <= 100000; t += 200) w.push(t - 200, t, 1.0);
    CHECK(w.size() <= RawRateWindow::kCap);
    CHECK(w.countsIn(100000, kWin) == doctest::Approx(6944.0 / 200.0).epsilon(0.001));
}
