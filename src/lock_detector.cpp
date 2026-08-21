#include "lock_detector.h"
#include <cstdlib>
namespace wind {
namespace {
constexpr int kRawActive  = 4;   // raw magnitude that counts as deliberate mouse motion
constexpr int kCursorMoved = 1;  // OS cursor moved at least this many px (it tracked input)
constexpr int kLockTicks  = 6;   // consecutive raw-active + cursor-frozen ticks -> lock
constexpr int kFreeTicks  = 3;   // consecutive cursor-moving ticks -> unlock
// Warp-anchor tell (issue #221). Tuned to the DOOM The Dark Ages field trace: warp jumps
// measured 90-550 px per tick landing on a <=6px cluster; a hand pans continuously and never
// re-lands on one pixel from 100px away. A slow (30fps) game warps only every ~5 ticks at
// 144Hz, so unlocking additionally requires the last warp landing to be >kWarpQuietTicks old -
// otherwise the hand-motion ticks BETWEEN warps would count as free evidence and flap the lock.
constexpr int kAnchorTolPx     = 6;    // "same position" tolerance
constexpr int kWarpJumpPx      = 100;  // a landing only counts as a warp if it came from afar
constexpr int kWarpLockReturns = 4;    // anchor landings before the lock engages
constexpr int kAnchorLossTicks = 45;   // ~0.3s off-anchor -> forget it (it was not a warp target)
constexpr int kWarpQuietTicks  = 12;   // warp this recent blocks the free-streak unlock
// Confinement-box tell (issue #221 round 2, Max: gentle mouselook needed ERRATIC motion to
// engage the anchor tell). Signature: a hand streaming plenty of mickeys while every cursor
// position stays inside a tiny box - gentle warping keeps the pointer jiggling around the
// recenter point. Precise desktop work never trips it: a careful hand produces proportionally
// FEW mickeys (ballistics map slow motion ~1:1), nowhere near kBoxRawSum in one window.
constexpr int kBoxTicks  = 24;   // tumbling window (~170ms at 144Hz) - engages fast
constexpr int kBoxRawSum = 400;  // mickeys accumulated in the window = deliberate sweeping
constexpr int kBoxSpanPx = 30;   // and the cursor went nowhere: confined
}

void LockDetector::reset() {
    locked_ = false; lockStreak_ = 0; freeStreak_ = 0;
    haveAnchor_ = false; anchorMissTicks_ = 0; warpReturns_ = 0; sinceWarp_ = 1000;
    boxTicks_ = 0; boxRawSum_ = 0;
}

bool LockDetector::update(bool clipConfined, int rawMag, int cursorMag) {
    return update(clipConfined, rawMag, cursorMag, false, 0, 0);
}

bool LockDetector::update(bool clipConfined, int rawMag, int cursorMag,
                          bool warpTell, int cx, int cy) {
    // Direct, reliable signal: a confined clip rect means a game has clipped the cursor.
    if (clipConfined) { locked_ = true; lockStreak_ = 0; freeStreak_ = 0; return locked_; }

    if (sinceWarp_ < 1000) sinceWarp_++;
    bool warpLanding = false;
    if (warpTell) {
        const bool nearAnchor = haveAnchor_ &&
            std::abs(cx - anchorX_) <= kAnchorTolPx && std::abs(cy - anchorY_) <= kAnchorTolPx;
        if (nearAnchor) {
            anchorMissTicks_ = 0;
            // Landing here FROM AFAR is a warp; merely resting here (weld dedupe, idle) is not.
            if (cursorMag >= kWarpJumpPx) {
                warpLanding = true;
                sinceWarp_ = 0;
                if (++warpReturns_ >= kWarpLockReturns) locked_ = true;
            }
        } else if (haveAnchor_) {
            if (++anchorMissTicks_ > kAnchorLossTicks) { haveAnchor_ = false; warpReturns_ = 0; }
        }
        if (!haveAnchor_ && cursorMag >= kWarpJumpPx) {
            // Only a position the cursor JUMPED to can be a warp target. A wrong pick (the far
            // end of a hand flick) never accumulates returns and ages out via anchorMissTicks_.
            anchorX_ = cx; anchorY_ = cy; haveAnchor_ = true;
            anchorMissTicks_ = 0; warpReturns_ = 0;
        }
        // Confinement-box tell: accumulate the tumbling window.
        if (boxTicks_ == 0) { boxMinX_ = boxMaxX_ = cx; boxMinY_ = boxMaxY_ = cy; boxRawSum_ = 0; }
        if (cx < boxMinX_) boxMinX_ = cx; if (cx > boxMaxX_) boxMaxX_ = cx;
        if (cy < boxMinY_) boxMinY_ = cy; if (cy > boxMaxY_) boxMaxY_ = cy;
        boxRawSum_ += rawMag;
        if (++boxTicks_ >= kBoxTicks) {
            if (boxRawSum_ >= kBoxRawSum &&
                boxMaxX_ - boxMinX_ <= kBoxSpanPx && boxMaxY_ - boxMinY_ <= kBoxSpanPx) {
                locked_ = true;
                if (warpReturns_ == 0) warpReturns_ = 1;   // report as warp-class for diagnostics
                sinceWarp_ = 0;                            // block the free-streak unlock nearby
                freeStreak_ = 0; lockStreak_ = 0;
            }
            boxTicks_ = 0;
        }
    }

    if (warpLanding) {
        // A warp landing is lock evidence, never free evidence.
        freeStreak_ = 0; lockStreak_ = 0;
    } else if (cursorMag >= kCursorMoved) {
        // The OS cursor is tracking input -> evidence of free movement. But a game that warps
        // every few ticks produces real hand motion BETWEEN warps - only a quiet spell since
        // the last warp landing makes the free streak trustworthy.
        freeStreak_++; lockStreak_ = 0;
        if (freeStreak_ >= kFreeTicks && sinceWarp_ > kWarpQuietTicks) {
            locked_ = false;
            warpReturns_ = 0;
        }
    } else if (rawMag >= kRawActive) {
        // Mouse moving but OS cursor frozen -> evidence of a lock.
        lockStreak_++; freeStreak_ = 0;
        if (lockStreak_ >= kLockTicks) locked_ = true;
    } else {
        // Idle (no significant input, cursor still): neither streak grows; hold current state.
        lockStreak_ = 0; freeStreak_ = 0;
    }
    return locked_;
}
}
