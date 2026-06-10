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

bool IsPresentEcho(bool presentedSinceLastFrame, unsigned accumulatedFrames,
                   unsigned dirtyCount, const GateRect& dirty0, const GateRect& overlay) {
    if (!presentedSinceLastFrame) return false;
    if (accumulatedFrames > 1) return false;   // merged composites could hide a real change
    if (dirtyCount != 1) return false;         // a real change elsewhere adds its own rect
    return dirty0.left == overlay.left && dirty0.top == overlay.top &&
           dirty0.right == overlay.right && dirty0.bottom == overlay.bottom;
}

void EchoFilter::noteRealChange() {
    // Cap well above the bypass threshold; the exact bound is irrelevant, it only has to never
    // overflow during an arbitrarily long full-rate run.
    constexpr int kStreakCap = 1000;
    if (realStreak_ < kStreakCap) ++realStreak_;
    timeoutRun_ = 0;
    skipDecay_  = 0;   // restart the decay window: 1:1 real/echo alternation never decays
    bypassRun_  = 0;
}

void EchoFilter::noteTimeout() {
    // Single timeouts interleave with full-rate content (the loop polls faster than DWM
    // composites); only an unbroken run long enough to be a real content gap resets.
    if (++timeoutRun_ >= kEchoTimeoutReset) {
        realStreak_ = 0;
        skipDecay_  = 0;
        bypassRun_  = 0;
        timeoutRun_ = kEchoTimeoutReset;   // clamp: an idle eternity must not overflow
    }
}

void EchoFilter::reset() { realStreak_ = 0; timeoutRun_ = 0; skipDecay_ = 0; bypassRun_ = 0; }

bool EchoFilter::onEchoShaped() {
    if (realStreak_ < kEchoBypassStreak) {
        // A SKIPPED echo is neutral to the timeout run AND decays the streak when echoes pile
        // up between reals. Both are load-bearing on an idle desktop (observed empirically):
        // every sparse rendered change spawns echoes of its own present, occasionally
        // misclassified as real (a late second echo, a merged accum>1 composite). If echoes
        // cleared the timeout run they would shield the streak from the reset; if they did not
        // decay it, those ~9/s fake reals out-ran the ~25/s timeouts and ratcheted the streak
        // to the threshold anyway. The genuine halved regime alternates real:echo 1:1 (the
        // decay window restarts on every real, so it never fires and the streak builds at full
        // speed); the idle echo stream outnumbers its fake reals ~13:1, decays ~6 per real,
        // and stays pinned at 0.
        if (++skipDecay_ >= kEchoSkipDecay) {
            skipDecay_ = 0;
            if (realStreak_ > 0) --realStreak_;
        }
        return false;
    }
    if (++bypassRun_ >= kEchoProbeInterval) {
        // Liveness probe: skip this one frame AND drop the streak just below the threshold so
        // the next event decides. A live game re-proves itself immediately (we skipped, so the
        // next composite is game-only with no echo expectation -> classified real -> streak
        // back over the threshold; total cost one held frame per interval). A stale chain has
        // nothing to re-prove with: it stops presenting here, its late-echo trickle (which
        // once defeated a plain probe by being misclassified real and re-arming a full
        // interval) decays the streak, and the timeout run resets it for good.
        bypassRun_  = 0;
        realStreak_ = kEchoBypassStreak - 1;
        skipDecay_  = 0;
        return false;
    }
    timeoutRun_ = 0;   // a BYPASSED echo is a full-rate cadence frame: stray single timeouts
    return true;       // (loop hitches) must not accumulate into a reset while engaged
}

}
