#include "doctest.h"
#include "../src/frame_gate.h"

using wind::GateRect;
using wind::FrameSnapshot;

TEST_CASE("RectsIntersect: overlapping rects intersect") {
    CHECK(wind::RectsIntersect(GateRect{0, 0, 100, 100}, GateRect{50, 50, 150, 150}));
}
TEST_CASE("RectsIntersect: touching edges do not intersect (half-open)") {
    CHECK_FALSE(wind::RectsIntersect(GateRect{0, 0, 100, 100}, GateRect{100, 0, 200, 100}));
    CHECK_FALSE(wind::RectsIntersect(GateRect{0, 0, 100, 100}, GateRect{0, 100, 100, 200}));
}
TEST_CASE("RectsIntersect: disjoint rects do not intersect") {
    CHECK_FALSE(wind::RectsIntersect(GateRect{0, 0, 10, 10}, GateRect{20, 20, 30, 30}));
}
TEST_CASE("RectsIntersect: empty rects never intersect") {
    CHECK_FALSE(wind::RectsIntersect(GateRect{50, 50, 50, 80}, GateRect{0, 0, 100, 100}));
    CHECK_FALSE(wind::RectsIntersect(GateRect{0, 0, 100, 100}, GateRect{10, 90, 90, 10}));
}
TEST_CASE("RectsIntersect: containment intersects") {
    CHECK(wind::RectsIntersect(GateRect{0, 0, 100, 100}, GateRect{40, 40, 60, 60}));
    CHECK(wind::RectsIntersect(GateRect{40, 40, 60, 60}, GateRect{0, 0, 100, 100}));
}

static FrameSnapshot Base() {
    FrameSnapshot s;
    s.level = 4.0;
    s.srcLeft = 720.0; s.srcTop = 405.0;
    s.cursorScreenX = 960.0; s.cursorScreenY = 540.0;
    s.cursorVisible = true;
    s.cursorShapeId = 0x1234;
    s.outlineAlpha = 1.0f;
    return s;
}

TEST_CASE("SnapshotsDiffer: identical snapshots do not differ") {
    CHECK_FALSE(wind::SnapshotsDiffer(Base(), Base()));
}
TEST_CASE("SnapshotsDiffer: source-rect epsilon (1e-3 desktop px)") {
    FrameSnapshot b = Base();
    b.srcLeft += 0.0005;                       // below epsilon: smoothing tail settles
    CHECK_FALSE(wind::SnapshotsDiffer(Base(), b));
    b.srcLeft = Base().srcLeft + 0.002;        // above epsilon: must render
    CHECK(wind::SnapshotsDiffer(Base(), b));
    FrameSnapshot c = Base(); c.srcTop += 0.002;
    CHECK(wind::SnapshotsDiffer(Base(), c));
}
TEST_CASE("SnapshotsDiffer: cursor epsilon (0.05 screen px)") {
    FrameSnapshot b = Base();
    b.cursorScreenX += 0.02;
    CHECK_FALSE(wind::SnapshotsDiffer(Base(), b));
    b.cursorScreenX = Base().cursorScreenX + 0.1;
    CHECK(wind::SnapshotsDiffer(Base(), b));
    FrameSnapshot c = Base(); c.cursorScreenY += 0.1;
    CHECK(wind::SnapshotsDiffer(Base(), c));
}
TEST_CASE("SnapshotsDiffer: zoom level change differs (ramp must render)") {
    FrameSnapshot b = Base(); b.level = 4.0001;
    CHECK(wind::SnapshotsDiffer(Base(), b));
}
TEST_CASE("SnapshotsDiffer: cursor visibility flip differs") {
    FrameSnapshot b = Base(); b.cursorVisible = false;
    CHECK(wind::SnapshotsDiffer(Base(), b));
}
TEST_CASE("SnapshotsDiffer: cursor shape change differs") {
    FrameSnapshot b = Base(); b.cursorShapeId = 0x9999;
    CHECK(wind::SnapshotsDiffer(Base(), b));
}
TEST_CASE("SnapshotsDiffer: outline fade alpha change differs") {
    FrameSnapshot b = Base(); b.outlineAlpha = 0.5f;
    CHECK(wind::SnapshotsDiffer(Base(), b));
}
TEST_CASE("SnapshotsDiffer: srcTop independently respects the epsilon") {
    FrameSnapshot b = Base(); b.srcTop += 0.0005;
    CHECK_FALSE(wind::SnapshotsDiffer(Base(), b));
}
TEST_CASE("SnapshotsDiffer: cursorScreenY independently respects the epsilon") {
    FrameSnapshot b = Base(); b.cursorScreenY += 0.02;
    CHECK_FALSE(wind::SnapshotsDiffer(Base(), b));
}
TEST_CASE("RectsIntersect: negative coordinates (multi-monitor) work") {
    CHECK(wind::RectsIntersect(GateRect{-200, -100, 0, 0}, GateRect{-50, -50, 50, 50}));
    CHECK_FALSE(wind::RectsIntersect(GateRect{-200, -100, -150, -50}, GateRect{0, 0, 50, 50}));
}

// IsPresentEcho: the DDA frame after our own Present arrives with one dirty rect equal to the
// capture-excluded overlay's rect; treating it as a desktop change chains present -> dirty ->
// present forever and idle frame-skip never engages (issue #96).
static const GateRect kOverlay{0, 0, 3840, 2160};

TEST_CASE("IsPresentEcho: exact signature is an echo") {
    CHECK(wind::IsPresentEcho(true, 1, 1, GateRect{0, 0, 3840, 2160}, kOverlay));
    CHECK(wind::IsPresentEcho(true, 0, 1, GateRect{0, 0, 3840, 2160}, kOverlay));   // accum 0 tolerated
}
TEST_CASE("IsPresentEcho: not an echo without a preceding present") {
    CHECK_FALSE(wind::IsPresentEcho(false, 1, 1, GateRect{0, 0, 3840, 2160}, kOverlay));
}
TEST_CASE("IsPresentEcho: merged composites are never an echo (may hide a real change)") {
    CHECK_FALSE(wind::IsPresentEcho(true, 2, 1, GateRect{0, 0, 3840, 2160}, kOverlay));
}
TEST_CASE("IsPresentEcho: more than one dirty rect means a real change rode along") {
    CHECK_FALSE(wind::IsPresentEcho(true, 1, 2, GateRect{0, 0, 3840, 2160}, kOverlay));
    CHECK_FALSE(wind::IsPresentEcho(true, 1, 0, GateRect{0, 0, 3840, 2160}, kOverlay));
}
TEST_CASE("IsPresentEcho: a partial-screen dirty rect is a real change, not an echo") {
    CHECK_FALSE(wind::IsPresentEcho(true, 1, 1, GateRect{100, 100, 200, 200}, kOverlay));
    CHECK_FALSE(wind::IsPresentEcho(true, 1, 1, GateRect{0, 0, 3840, 2159}, kOverlay));   // off by 1
    CHECK_FALSE(wind::IsPresentEcho(true, 1, 1, GateRect{1, 0, 3840, 2160}, kOverlay));
}
// EchoFilter: real-change streak hysteresis. Full-rate fullscreen content can ALIAS with the
// echo signature (its full-monitor dirty merges into the same composite as our echo); without
// the bypass the gate would skip every other real frame (~72 fps capture on a 144 Hz panel).

TEST_CASE("EchoFilter: idle convergence - sparse reals plus echoes never engage the bypass") {
    wind::EchoFilter f;
    // Idle desktop: an occasional real change (clock tick), its one echo afterwards, then
    // timeouts until the next change. The streak never approaches the threshold.
    for (int burst = 0; burst < 20; ++burst) {
        f.noteRealChange();                       // sparse real change
        CHECK_FALSE(f.onEchoShaped());            // its echo is classified echo (skipped)
        for (int t = 0; t < 10; ++t) f.noteTimeout();   // static desktop between changes
        CHECK(f.realStreak() == 0);
        CHECK_FALSE(f.onEchoShaped());
    }
}

TEST_CASE("EchoFilter: halved regime (real, echo-shaped, timeout cycle) engages the bypass") {
    wind::EchoFilter f;
    // The cycle measured in wind_diag.log: the loop polls ~200 Hz against 144 Hz composites, so
    // while halved each game frame is followed by an echo-shaped merge and ONE poll timeout.
    // Echo-shaped events and single timeouts MUST NOT reset the streak or the bypass could
    // never engage (this exact pattern pinned the streak at 1 with reset-on-first-timeout).
    int firstBypassedCycle = -1;
    for (int cyc = 1; cyc <= 16; ++cyc) {
        f.noteRealChange();                       // game-only composite (we skipped: no echo)
        if (f.onEchoShaped() && firstBypassedCycle < 0) firstBypassedCycle = cyc;
        f.noteTimeout();                          // poll outpaces the next composite
    }
    // The 8th cycle's real change reaches the threshold, so its echo-shaped frame is bypassed.
    CHECK(firstBypassedCycle == wind::kEchoBypassStreak);
    CHECK(f.onEchoShaped());                      // and it stays engaged while reals keep coming
}

TEST_CASE("EchoFilter: strict real/echo-shaped alternation engages the bypass too") {
    wind::EchoFilter f;
    int firstBypassedEvent = -1;
    for (int ev = 1; ev <= 32; ++ev) {
        if (ev % 2 == 1) f.noteRealChange();
        else if (f.onEchoShaped() && firstBypassedEvent < 0) firstBypassedEvent = ev;
    }
    CHECK(firstBypassedEvent == 2 * wind::kEchoBypassStreak);
}

TEST_CASE("EchoFilter: one skipped echo per real and bypassed echoes never move the streak") {
    wind::EchoFilter f;
    for (int i = 0; i < 3; ++i) f.noteRealChange();
    CHECK_FALSE(f.onEchoShaped());                // below threshold: classified echo...
    CHECK(f.realStreak() == 3);                   // ...without moving the streak (1:1 ratio)
    for (int i = 0; i < 20; ++i) f.noteRealChange();
    const int streak = f.realStreak();
    CHECK(f.onEchoShaped());                      // above threshold: bypassed...
    CHECK(f.realStreak() == streak);              // ...still without moving the streak
}

TEST_CASE("EchoFilter: echo pile-up between reals decays the streak (ratio filter)") {
    wind::EchoFilter f;
    for (int i = 0; i < 6; ++i) f.noteRealChange();
    // The observed idle ratio: ~13 skipped echoes per misclassified real. The pile-up decays
    // the streak (-1 per kEchoSkipDecay echoes since the last real) faster than reals feed it,
    // so this pattern can repeat forever without ever engaging the bypass.
    for (int e = 0; e < 13; ++e) CHECK_FALSE(f.onEchoShaped());
    CHECK(f.realStreak() == 0);
    for (int i = 0; i < 200; ++i) {
        f.noteRealChange();
        for (int e = 0; e < 13; ++e) CHECK_FALSE(f.onEchoShaped());
    }
    CHECK(f.realStreak() == 0);
}

TEST_CASE("EchoFilter: recovery - a full timeout run resets, a single timeout does not") {
    wind::EchoFilter f;
    for (int i = 0; i < 50; ++i) f.noteRealChange();   // sustained full-rate content
    CHECK(f.onEchoShaped());
    // A short gap (a game hitch, or the poll simply outpacing composites) must keep the bypass.
    for (int t = 0; t < wind::kEchoTimeoutReset - 1; ++t) f.noteTimeout();
    CHECK(f.onEchoShaped());                      // gap too short: still engaged
    CHECK(f.realStreak() == 50);                  // the echo-shaped frame cleared the run
    // An unbroken run of kEchoTimeoutReset timeouts is a real content gap: streak resets.
    for (int t = 0; t < wind::kEchoTimeoutReset; ++t) f.noteTimeout();
    CHECK(f.realStreak() == 0);
    CHECK_FALSE(f.onEchoShaped());                // echoes are skipped again
    f.noteRealChange();                           // one fresh change does not re-engage
    CHECK_FALSE(f.onEchoShaped());
}

TEST_CASE("EchoFilter: skipped echoes do not shield the streak from the timeout reset") {
    wind::EchoFilter f;
    // The empirically-observed idle trap: sparse desktop changes render, each present spawns
    // echo frames that interleave the idle timeouts. Skipped echoes are NEUTRAL to the timeout
    // run, so it still accumulates through them and the streak resets between sparse changes -
    // the bypass must never engage no matter how long this pattern repeats.
    for (int i = 0; i < 100; ++i) {
        f.noteRealChange();                       // sparse change (clock tick, tray repaint)
        CHECK_FALSE(f.onEchoShaped());            // echo of our response present: skipped
        f.noteTimeout(); f.noteTimeout();
        CHECK_FALSE(f.onEchoShaped());            // a late second echo: skipped, still neutral
        f.noteTimeout(); f.noteTimeout();         // 4th CONSECUTIVE-through-echoes timeout
        CHECK(f.realStreak() == 0);               // -> the run reset the streak
    }
}

TEST_CASE("EchoFilter: engaged bypass survives stray single timeouts between frames") {
    wind::EchoFilter f;
    for (int i = 0; i < wind::kEchoBypassStreak; ++i) f.noteRealChange();
    // Steady engaged cadence with an occasional poll outpacing the composite: bypassed echoes
    // clear the timeout run, so strays never accumulate into a reset. Only the probes skip,
    // and the live game re-proves on the composite right after each (modeled by the real).
    int skipped = 0;
    for (int i = 0; i < 1000; ++i) {
        if (!f.onEchoShaped()) { ++skipped; f.noteRealChange(); }
        if (i % 5 == 0) f.noteTimeout();
    }
    CHECK(skipped == 1000 / (wind::kEchoProbeInterval + 1));
    CHECK(f.realStreak() >= wind::kEchoBypassStreak);   // never reset while engaged
}

TEST_CASE("EchoFilter: probe - a live game re-proves and pays one held frame per interval") {
    wind::EchoFilter f;
    for (int i = 0; i < wind::kEchoBypassStreak; ++i) f.noteRealChange();
    // Post-bypass aliasing world: EVERY composite is echo-shaped (game dirt merged with our
    // echo). The probe skips one frame and demands re-proof; the game supplies it at once
    // (the post-probe composite carries no echo expectation -> a real change).
    int probes = 0;
    for (int i = 0; i < 1000; ++i) {
        if (!f.onEchoShaped()) { ++probes; f.noteRealChange(); }
    }
    CHECK(probes == 1000 / (wind::kEchoProbeInterval + 1));   // exactly one per interval
    CHECK(f.onEchoShaped());                      // still engaged at the end
}

TEST_CASE("EchoFilter: probe - a stale chain cannot ride the old streak past the probe") {
    wind::EchoFilter f;
    for (int i = 0; i < wind::kEchoBypassStreak; ++i) f.noteRealChange();
    // Content stopped right after engaging: only our own echoes keep arriving. They ride the
    // streak up to the probe...
    for (int i = 0; i < wind::kEchoProbeInterval - 1; ++i) CHECK(f.onEchoShaped());
    CHECK_FALSE(f.onEchoShaped());                // ...which skips and demands re-proof
    // No content = no re-proof: the late-echo trickle is skipped (and decays the streak)
    // instead of being ridden for another interval.
    CHECK_FALSE(f.onEchoShaped());
    CHECK_FALSE(f.onEchoShaped());
    // And once nothing presents anymore, the timeout run resets the streak for good.
    for (int t = 0; t < wind::kEchoTimeoutReset; ++t) f.noteTimeout();
    CHECK(f.realStreak() == 0);
    CHECK_FALSE(f.onEchoShaped());
}

TEST_CASE("EchoFilter: reset discards a stale streak (fresh zoom-in context)") {
    wind::EchoFilter f;
    for (int i = 0; i < 50; ++i) f.noteRealChange();
    CHECK(f.onEchoShaped());
    f.reset();
    CHECK(f.realStreak() == 0);
    CHECK_FALSE(f.onEchoShaped());
}

TEST_CASE("EchoFilter: counters hold under arbitrarily long runs (no overflow)") {
    wind::EchoFilter f;
    for (int i = 0; i < 2'000'000; ++i) f.noteRealChange();   // hours of 144 fps content
    CHECK(f.realStreak() > 0);
    CHECK(f.realStreak() <= 1000);                // capped, never wrapped negative
    CHECK(f.onEchoShaped());
    for (int i = 0; i < 2'000'000; ++i) f.noteTimeout();      // hours of idle
    CHECK(f.realStreak() == 0);
    CHECK_FALSE(f.onEchoShaped());
    f.noteRealChange();                           // and the filter still responds normally
    CHECK(f.realStreak() == 1);
}

TEST_CASE("IsPresentEcho: echo suppression breaks the present->dirty->present loop") {
    // Simulate the idle feedback: each present makes the NEXT frame arrive as the echo signature.
    // With suppression, frame 1 is classified echo -> no present -> no further dirty frames.
    bool presented = true;                       // tick 0 presented (last real change)
    int presents = 0;
    for (int tick = 1; tick <= 10; ++tick) {
        bool dirtyArrived = presented;           // echo of the previous tick's present
        presented = false;
        if (dirtyArrived &&
            !wind::IsPresentEcho(true, 1, 1, GateRect{0, 0, 3840, 2160}, kOverlay)) {
            presents++;                          // gate saw a "change" -> would present again
            presented = true;
        }
    }
    CHECK(presents == 0);                        // loop broken on the first echo
}
