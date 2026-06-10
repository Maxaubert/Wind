#include "frame_gate.h"
#include <cmath>

namespace wind {

bool RectsIntersect(const GateRect& a, const GateRect& b) {
    if (a.right <= a.left || a.bottom <= a.top) return false;
    if (b.right <= b.left || b.bottom <= b.top) return false;
    return a.left < b.right && b.left < a.right &&
           a.top < b.bottom && b.top < a.bottom;
}

bool SnapshotsDiffer(const FrameSnapshot& a, const FrameSnapshot& b) {
    constexpr double kSrcEps    = 1e-3;   // desktop px; * maxLevel 12 = 0.012 screen px
    constexpr double kCursorEps = 0.05;   // screen px
    constexpr double kLevelEps  = 1e-9;   // effectively exact: any ramp step renders
    if (std::fabs(a.level - b.level) > kLevelEps) return true;
    if (std::fabs(a.srcLeft - b.srcLeft) > kSrcEps) return true;
    if (std::fabs(a.srcTop - b.srcTop) > kSrcEps) return true;
    if (std::fabs(a.cursorScreenX - b.cursorScreenX) > kCursorEps) return true;
    if (std::fabs(a.cursorScreenY - b.cursorScreenY) > kCursorEps) return true;
    if (a.cursorVisible != b.cursorVisible) return true;
    if (a.cursorShapeId != b.cursorShapeId) return true;
    if (a.outlineAlpha != b.outlineAlpha) return true;
    return false;
}

bool IsPresentEcho(bool presentedSinceLastFrame, long long dirtyArea, long long overlayArea) {
    if (!presentedSinceLastFrame) return false;
    if (overlayArea <= 0) return false;
    // Coverage threshold, not exact rect equality (see the header: the echo arrives clipped by
    // higher-band windows and/or split into multiple rects). A merged-in real change is rescued
    // by the EchoFilter grace, not by rect/accum forensics.
    return dirtyArea * kEchoCoverageDen >= overlayArea * kEchoCoverageNum;
}

// Streak + grace hybrid (see frame_gate.h for the full rationale).
void EchoFilter::noteRealChange() {
    // Cap well above the bypass threshold; the exact bound is irrelevant, it only has to never
    // overflow during an arbitrarily long full-rate run.
    constexpr int kStreakCap = 1000;
    if (realStreak_ < kStreakCap) ++realStreak_;
    timeoutRun_ = 0;
    skipDecay_  = 0;                  // restart the decay window
    bypassRun_  = 0;
    graceLeft_  = kEchoGraceTicks;    // a merged follow-up may ride this change's present echo
}

void EchoFilter::noteTimeout() {
    // NB: timeouts do NOT burn the grace - it is a per-change budget of echo-shaped frames, not
    // a wall-clock window (burning it on timeouts halved its effective size, since the loop
    // polls faster than DWM composites and timeouts interleave 1:1 with frames).
    // Single timeouts interleave with full-rate content; only an unbroken run long enough to be
    // a real content gap resets the streak.
    if (++timeoutRun_ >= kEchoTimeoutReset) {
        realStreak_ = 0;
        skipDecay_  = 0;
        bypassRun_  = 0;
        graceLeft_  = 0;                   // the gap ended the change's follow-ups: budget over
        timeoutRun_ = kEchoTimeoutReset;   // clamp: an idle eternity must not overflow
    }
}

void EchoFilter::reset() {
    realStreak_ = 0; timeoutRun_ = 0; skipDecay_ = 0; bypassRun_ = 0; graceLeft_ = 0;
}

bool EchoFilter::onEchoShaped() {
    // Every echo-shaped frame burns the grace regardless of branch: it must measure distance
    // from the last REAL change only, or leftover grace would let a stale post-probe chain
    // ride past the probe's re-proof demand.
    const bool gracedThisFrame = graceLeft_ > 0;
    if (gracedThisFrame) --graceLeft_;
    if (realStreak_ >= kEchoBypassStreak) {
        // STREAK bypass engaged: sustained full-rate content; render every composite.
        if (++bypassRun_ >= kEchoProbeInterval) {
            // Liveness probe: skip this one frame AND drop the streak just below the threshold
            // so the next event decides. A live game re-proves itself on the next composite
            // (game-only, no echo pending -> real -> re-engaged), and the engine's catch-up
            // render shows this skipped frame one tick late, so the probe costs ~nothing. A
            // stale chain stops presenting here; its late-echo trickle decays the streak and
            // the timeout run resets it for good.
            bypassRun_  = 0;
            realStreak_ = kEchoBypassStreak - 1;
            skipDecay_  = 0;
            return false;
        }
        timeoutRun_ = 0;   // a bypassed echo is a full-rate cadence frame: stray single
        return true;       // timeouts must not accumulate into a reset while engaged
    }
    if (gracedThisFrame) {
        // GRACE bypass: a real change happened within the last kEchoGraceTicks events, so this
        // frame may be that activity's follow-up merged into our echo - render it. Burning
        // (never refreshing) the grace is what kills the pure-echo chain; clearing the timeout
        // run mirrors the streak bypass (these are presents at composite cadence, and the
        // interleaved single timeouts must not reset a streak that is mid-build).
        timeoutRun_ = 0;
        return true;
    }
    // SKIPPED as an echo. Neutral to the timeout run AND decays the streak when echoes pile up
    // between reals (both load-bearing on an idle desktop: if skipped echoes cleared the run
    // they would shield the streak from the reset; if they did not decay it, sparse fake reals
    // out-ran the timeouts and ratcheted the streak to the threshold).
    if (++skipDecay_ >= kEchoSkipDecay) {
        skipDecay_ = 0;
        if (realStreak_ > 0) --realStreak_;
    }
    return false;
}

}
