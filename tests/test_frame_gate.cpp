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

// IsPresentEcho: the DDA frame after our own Present arrives with a dirty region covering the
// capture-excluded overlay; treating it as a desktop change chains present -> dirty -> present
// forever and idle frame-skip never engages (issue #96). The signature is COVERAGE-based (the
// echo arrives as the exact overlay rect, clipped by the taskbar's band to ~95%, or split into
// rects) and deliberately ignores AccumulatedFrames (our own echoes merge into accum>1
// composites with the same shape); a merged-in real change is arbitrated by the grace, not by
// rect forensics.
static const long long kOverlayArea = 3840LL * 2160LL;

TEST_CASE("IsPresentEcho: full-overlay dirty coverage is echo-shaped") {
    CHECK(wind::IsPresentEcho(true, kOverlayArea, kOverlayArea));
}
TEST_CASE("IsPresentEcho: a taskbar-clipped echo (~95% coverage) is still echo-shaped") {
    CHECK(wind::IsPresentEcho(true, 3840LL * 2050LL, kOverlayArea));   // overlay minus a 110px strip
}
TEST_CASE("IsPresentEcho: coverage exactly at the threshold is echo-shaped") {
    const long long t = kOverlayArea * wind::kEchoCoverageNum / wind::kEchoCoverageDen;
    CHECK(wind::IsPresentEcho(true, t, kOverlayArea));
    CHECK_FALSE(wind::IsPresentEcho(true, t - wind::kEchoCoverageDen, kOverlayArea));
}
TEST_CASE("IsPresentEcho: not an echo without a preceding present") {
    CHECK_FALSE(wind::IsPresentEcho(false, kOverlayArea, kOverlayArea));
}
TEST_CASE("IsPresentEcho: a small dirty region is a real change, not an echo") {
    CHECK_FALSE(wind::IsPresentEcho(true, 100LL * 100LL, kOverlayArea));          // a caret/dialog
    CHECK_FALSE(wind::IsPresentEcho(true, 1920LL * 1080LL, kOverlayArea));        // quarter screen
    CHECK_FALSE(wind::IsPresentEcho(true, kOverlayArea / 2, kOverlayArea));       // half screen
}
TEST_CASE("IsPresentEcho: zero dirty area / degenerate overlay never classify as echo") {
    CHECK_FALSE(wind::IsPresentEcho(true, 0, kOverlayArea));
    CHECK_FALSE(wind::IsPresentEcho(true, 100, 0));
}
// EchoFilter: streak + grace hybrid. The STREAK keeps sustained full-rate content (which merges
// with our echo every composite) at the full refresh rate; the GRACE rescues a sporadic
// transition's follow-up repaints that merged into the echo of the present they triggered
// (alt-tab commit, Start menu close - the swallow the streak alone had, issue #96); skipped
// echoes additionally schedule a one-shot catch-up render in the engine (not modeled here).

TEST_CASE("EchoFilter: a quiet desktop's echo is skipped (no streak, no recent activity)") {
    wind::EchoFilter f;
    CHECK_FALSE(f.onEchoShaped());                // nothing ever changed: pure echo
    for (int t = 0; t < 50; ++t) f.noteTimeout();
    CHECK_FALSE(f.onEchoShaped());
}

TEST_CASE("EchoFilter: sporadic swallow case - a transition's merged repaints render (grace)") {
    wind::EchoFilter f;
    // The start-menu-close sequence from the repro log: the close triggers a real partial-rect
    // change (rendered; we present), and the follow-up full-screen repaints land in the same
    // DWM composites as our echoes - echo-shaped. The streak machinery alone skipped them
    // (streak < threshold for a sporadic event) and the stale menu stuck on screen until the
    // next unrelated change. The grace must render them.
    for (int t = 0; t < 200; ++t) f.noteTimeout();   // long-idle desktop
    f.noteRealChange();                              // menu close: partial-rect real change
    CHECK(f.onEchoShaped());                         // merged follow-up repaint: MUST render
    CHECK(f.onEchoShaped());                         // and the next one (grace >= 2)
}

TEST_CASE("EchoFilter: the post-present echo chain dies within the grace window") {
    wind::EchoFilter f;
    f.noteRealChange();
    // Each echo-render presents again and spawns the next echo; only burning (never refreshing)
    // the grace on echo-shaped frames kills the chain. Bounded: exactly kEchoGraceTicks (the
    // streak is far below its threshold for an isolated change).
    int rendered = 0;
    while (f.onEchoShaped() && rendered < 1000) ++rendered;
    CHECK(rendered == wind::kEchoGraceTicks);
    CHECK_FALSE(f.onEchoShaped());                // chain dead: idle skipping resumes
}

TEST_CASE("EchoFilter: the grace survives short timeout runs but a content gap clears it") {
    wind::EchoFilter f;
    f.noteRealChange();
    // Single interleaved timeouts (the poll outpacing composites) must not eat the budget...
    for (int t = 0; t < wind::kEchoTimeoutReset - 1; ++t) f.noteTimeout();
    CHECK(f.graceLeft() == wind::kEchoGraceTicks);
    CHECK(f.onEchoShaped());                      // a late merged follow-up still renders
    // ...but a full content-gap run means the change's follow-ups are over: budget cleared.
    f.noteRealChange();
    for (int t = 0; t < wind::kEchoTimeoutReset; ++t) f.noteTimeout();
    CHECK(f.graceLeft() == 0);
    CHECK_FALSE(f.onEchoShaped());                // an echo after the gap: skipped
}

TEST_CASE("EchoFilter: post-skip suspect stretches do not stall the streak build-up") {
    wind::EchoFilter f;
    // The fully-merged full-rate build-up measured on the HDR panel: each cycle is one real
    // (post-suspect-stretch game frame), kEchoGraceTicks graced merges, then 4 skips - one
    // grace-exhausted skip (which triggers the catch-up) plus the ~3-frame echo-budget drain.
    // The skips must not decay the streak faster than the reals build it (kEchoSkipDecay > 4),
    // or the bypass never engages and the game stays at ~60% rate forever.
    int cycles = 0;
    while (f.realStreak() < wind::kEchoBypassStreak && cycles < 100) {
        ++cycles;
        f.noteRealChange();
        for (int g = 0; g < wind::kEchoGraceTicks; ++g) { CHECK(f.onEchoShaped()); f.noteTimeout(); }
        for (int s = 0; s < 4; ++s) { (void)f.onEchoShaped(); f.noteTimeout(); }   // suspect skips
    }
    CHECK(cycles <= wind::kEchoBypassStreak);     // engaged within ~8 cycles (~0.5 s)
    CHECK(f.onEchoShaped());                      // and full rate from then on
}

TEST_CASE("EchoFilter: ambient few-Hz desktop changes never engage the bypass") {
    wind::EchoFilter f;
    // A wallpaper repaint / blinking caret at a few Hz: real change, its grace-rendered echo
    // tail, one skipped echo, then a quiet gap. The streak must reset every cycle (the timeout
    // run accumulates through the SKIPPED echoes) so this never escalates to the full-rate
    // bypass no matter how long it repeats - this exact pattern, amplified by a grace-only
    // filter without the streak's gap reset, once kept the chain alive indefinitely.
    for (int cyc = 0; cyc < 200; ++cyc) {
        f.noteRealChange();
        for (int g = 0; g < wind::kEchoGraceTicks; ++g) CHECK(f.onEchoShaped());
        CHECK_FALSE(f.onEchoShaped());            // grace burnt: echo skipped
        for (int t = 0; t < 30; ++t) f.noteTimeout();   // quiet until the next ambient change
        CHECK(f.realStreak() == 0);               // gap reset: bypass never engages
        CHECK_FALSE(f.onEchoShaped());
    }
}

TEST_CASE("EchoFilter: halved regime (real, echo-shaped, timeout cycle) renders full rate and"
          " engages the streak bypass") {
    wind::EchoFilter f;
    // The cycle measured in wind_diag.log: the loop polls ~200 Hz against 144 Hz composites, so
    // each game frame is followed by an echo-shaped merge and ONE poll timeout. The grace keeps
    // the merges rendering from the FIRST cycle (no halving even during build-up), and the
    // accumulating reals engage the streak bypass for the long run.
    for (int cyc = 0; cyc < 2000; ++cyc) {
        f.noteRealChange();                       // game-only composite
        CHECK(f.onEchoShaped());                  // merged composite: renders (grace or streak)
        f.noteTimeout();                          // poll outpaces the next composite
    }
    CHECK(f.realStreak() >= wind::kEchoBypassStreak);   // bypass engaged and held
}

TEST_CASE("EchoFilter: engaged bypass survives stray single timeouts between frames") {
    wind::EchoFilter f;
    for (int i = 0; i < wind::kEchoBypassStreak; ++i) f.noteRealChange();
    // Steady engaged cadence with an occasional poll outpacing the composite: bypassed echoes
    // clear the timeout run, so strays never accumulate into a reset. Only the probes skip,
    // and the live game re-proves on the composite right after each (modeled by the real).
    int skipped = 0;
    for (int i = 0; i < 2000; ++i) {
        if (!f.onEchoShaped()) { ++skipped; f.noteRealChange(); }
        if (i % 5 == 0) f.noteTimeout();
    }
    CHECK(skipped == 2000 / (wind::kEchoProbeInterval + 1));
    CHECK(f.realStreak() >= wind::kEchoBypassStreak);   // never reset while engaged
}

TEST_CASE("EchoFilter: probe - a live game re-proves and pays one skip per interval") {
    wind::EchoFilter f;
    for (int i = 0; i < wind::kEchoBypassStreak; ++i) f.noteRealChange();
    // Post-bypass aliasing world: EVERY composite is echo-shaped (game dirt merged with our
    // echo). The probe skips one frame and demands re-proof; the game supplies it at once
    // (the post-probe composite carries no echo expectation -> a real change). The engine's
    // catch-up render additionally shows the probe-skipped frame one tick late.
    int probes = 0;
    for (int i = 0; i < 2000; ++i) {
        if (!f.onEchoShaped()) { ++probes; f.noteRealChange(); }
    }
    CHECK(probes == 2000 / (wind::kEchoProbeInterval + 1));   // exactly one per interval
    CHECK(f.onEchoShaped());                      // still engaged at the end
}

TEST_CASE("EchoFilter: probe - a stale chain cannot ride the old streak past the probe") {
    wind::EchoFilter f;
    for (int i = 0; i < wind::kEchoBypassStreak; ++i) f.noteRealChange();
    // Content stopped right after engaging: only our own echoes keep arriving. They ride the
    // streak up to the probe... (the last real change's grace burns first, then streak bypass)
    for (int i = 0; i < wind::kEchoProbeInterval - 1; ++i) {
        CHECK(f.onEchoShaped());
    }
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

TEST_CASE("EchoFilter: recovery - a full timeout run resets the streak, a single one does not") {
    wind::EchoFilter f;
    for (int i = 0; i < 50; ++i) f.noteRealChange();   // sustained full-rate content
    CHECK(f.onEchoShaped());
    // A short gap (a game hitch, or the poll simply outpacing composites) must keep the bypass.
    for (int t = 0; t < wind::kEchoTimeoutReset - 1; ++t) f.noteTimeout();
    CHECK(f.onEchoShaped());                      // gap too short: still engaged
    CHECK(f.realStreak() == 50);                  // the bypassed echo cleared the run
    // An unbroken run of kEchoTimeoutReset timeouts is a real content gap: streak resets.
    for (int t = 0; t < wind::kEchoTimeoutReset; ++t) f.noteTimeout();
    CHECK(f.realStreak() == 0);
    CHECK_FALSE(f.onEchoShaped());                // echoes are skipped again (grace burnt long ago)
}

TEST_CASE("EchoFilter: off-view activity is neutral - it cannot open grace or build streak") {
    wind::EchoFilter f;
    // The engine never feeds off-view changes (frameChangesView returns before noteRealChange),
    // so from the filter's perspective an off-view animation is timeouts + occasional echoes.
    f.noteRealChange();                           // one view-intersecting change
    for (int t = 0; t < 20; ++t) f.noteTimeout(); // quiet gap: streak and grace both cleared
    for (int i = 0; i < 100; ++i) CHECK_FALSE(f.onEchoShaped());   // never re-opens by itself
}

TEST_CASE("EchoFilter: reset discards stale evidence (fresh zoom-in context)") {
    wind::EchoFilter f;
    for (int i = 0; i < 50; ++i) f.noteRealChange();
    CHECK(f.onEchoShaped());
    f.reset();
    CHECK(f.realStreak() == 0);
    CHECK(f.graceLeft() == 0);
    CHECK_FALSE(f.onEchoShaped());
}

TEST_CASE("EchoFilter: counters hold under arbitrarily long runs (no overflow/underflow)") {
    wind::EchoFilter f;
    for (int i = 0; i < 2'000'000; ++i) f.noteRealChange();   // hours of 144 fps content
    CHECK(f.realStreak() > 0);                                // capped, never wrapped negative
    CHECK(f.graceLeft() == wind::kEchoGraceTicks);
    CHECK(f.onEchoShaped());
    for (int i = 0; i < 2'000'000; ++i) f.noteTimeout();      // hours of idle
    CHECK(f.realStreak() == 0);
    CHECK(f.graceLeft() == 0);                                // clamped at zero
    CHECK_FALSE(f.onEchoShaped());
    f.noteRealChange();                           // and the filter still responds normally
    CHECK(f.realStreak() == 1);
    CHECK(f.graceLeft() == wind::kEchoGraceTicks);
}

TEST_CASE("IsPresentEcho + EchoFilter: the present->dirty->present loop is bounded and merged"
          " transition repaints render") {
    // Drive the engine's actual feedback shape (signature + grace together). World model: every
    // present we make echoes back exactly one composite later as ONE full-overlay dirty rect;
    // a tick that skipped presents nothing, so no echo follows it; an isolated real desktop
    // change arrives as a partial dirty rect, and a transition's FINAL repaint merges into the
    // same composite as our echo (full-overlay rect riding an armed echo expectation - the
    // start-menu-close shape that the old streak machinery swallowed).
    wind::EchoFilter f;
    int presents = 0;
    bool presented = false;                       // did the previous tick present (echo pending)?
    bool mergedRepaintRendered = false;
    int chainDeadTick = -1;                       // first tick the desktop goes fully quiet
    for (int tick = 0; tick < 200; ++tick) {
        const bool realChange = (tick == 0);                 // the transition's first repaint
        const bool mergedRepaint = (tick == 1);              // its final repaint, riding the echo
        const bool echoPending = presented;
        presented = false;
        if (!realChange && !mergedRepaint && !echoPending) { // nothing composited this tick
            f.noteTimeout();
            if (chainDeadTick < 0) chainDeadTick = tick;
            continue;
        }
        const long long dirtyArea = realChange ? 100LL * 100LL : kOverlayArea;
        bool render;
        if (wind::IsPresentEcho(echoPending, dirtyArea, kOverlayArea)) {
            render = f.onEchoShaped();            // echo-shaped: the grace decides
            if (render && mergedRepaint) mergedRepaintRendered = true;
        } else {
            f.noteRealChange();
            render = true;
        }
        if (render) { presents++; presented = true; }
    }
    CHECK(mergedRepaintRendered);                 // the swallowed-transition bug stays fixed
    // The echo chain is BOUNDED: the real change + merged repaint render, then at most
    // kEchoGraceTicks echo-renders before the chain dies and the desktop goes quiet forever.
    CHECK(presents <= 2 + wind::kEchoGraceTicks);
    CHECK(chainDeadTick > 0);
    CHECK(chainDeadTick <= 3 + wind::kEchoGraceTicks);
}
