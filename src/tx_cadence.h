#pragma once
// Transform write cadence (issue #204) - PURE, no <windows.h>, so it is unit-testable.
//
// Traced against native Windows Magnifier with tools/magtrace.ps1 (which reads the one global
// desktop magnification state via MagGetFullscreenTransform, so it records what the ACTIVE
// magnifier writes). Split by regime:
//
//                      native      Wind (before)
//   ramping            59.1/s      119.8/s     interval median 14.47ms vs 6.94ms
//   panning            48.6/s       91.7/s     offset step median 2.24px vs 1.41px
//   1px-only writes    9.5%         33.6%
//
// We wrote about twice as often, at half the granularity, because we wrote once per tick on a
// 144Hz panel. Our INTERVALS were more regular than native's (p95 7.56ms vs 31.44ms), so the
// surplus was not buying smoothness - each write makes DWM redo work proportional to the zoom
// level, and we were simply saturating the compositor.
//
// These gates COALESCE; they never drop a destination state. The tick loop recomputes the target
// from the mapper every tick, so a suppressed write is superseded by a fresher one rather than
// lost. The two escapes below make that guarantee complete.
namespace wind {

struct TxCadenceIn {
    bool   changed = false;          // the computed transform differs from what DWM last got
    bool   levelMoved = false;       // ...and the difference includes the LEVEL
    bool   rampStopped = false;      // the controller stopped requesting new levels
    double applyLevel = 1.0;         // the level about to be applied
    int    dMoveDest = 0;            // max(|dtx|,|dty|), DESTINATION (screen) pixels
    unsigned long long sinceLastWriteMs = 0;
    int    writeHz = 60;             // 0 = uncapped (per tick)
    int    minOffsetPx = 2;          // 0 = write every change
    unsigned long long settleMs = 100;
};

inline bool ShouldWriteTransform(const TxCadenceIn& in) {
    if (!in.changed) return false;
    // The 1x reset is never rate-limited: it runs at teardown, and deferring it would leave the
    // desktop magnified for an extra tick (or forever, if no further tick writes).
    if (in.applyLevel <= 1.0) return true;

    bool gated = false;
    if (in.writeHz > 0) {
        const unsigned long long minGap = 1000ull / (unsigned long long)in.writeHz;
        if (in.sinceLastWriteMs < minGap) gated = true;
    }
    // Granularity applies to PAN-only writes. A level change is never held back for being small:
    // level and geometry must stay consistent or the view scales without re-centring.
    if (!gated && !in.levelMoved && in.minOffsetPx > 0 && in.dMoveDest < in.minOffsetPx)
        gated = true;

    // Escape 1: nothing is held back indefinitely. Without this a residual sub-threshold movement
    // at the end of a pan would never be written and the view would rest up to minOffsetPx away
    // from where the cursor actually is.
    if (gated && in.sinceLastWriteMs >= in.settleMs) gated = false;
    // Escape 2: the final level of a stopped ramp always lands exactly, so a zoom never settles a
    // fraction off the level the user asked for.
    if (gated && in.levelMoved && in.rampStopped) gated = false;

    return !gated;
}

}  // namespace wind
