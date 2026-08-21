#include "lock_detector.h"
#include <cstdlib>
namespace wind {
namespace {
constexpr int kCursorMoved = 1;  // OS cursor moved at least this many px (it tracked input)
// Warp-anchor tell (issue #221). Tuned to the DOOM The Dark Ages field trace: warp jumps
// measured 90-550 px per tick landing on a <=6px cluster; a hand pans continuously and never
// re-lands on one pixel from 100px away. A slow (30fps) game warps only every ~5 ticks at
// 144Hz, so unlocking additionally requires the last warp landing to be >warpQuietTicks_ old -
// otherwise the hand-motion ticks BETWEEN warps would count as free evidence and flap the lock.
constexpr int kAnchorTolPx     = 6;    // "same position" tolerance
constexpr int kWarpJumpPx      = 100;  // a landing only counts as a warp if it came from afar
constexpr int kWarpLockReturns = 4;    // anchor landings before the lock engages
// Confinement-box tell (issue #221 round 2, Max: gentle mouselook needed ERRATIC motion to
// engage the anchor tell). Signature: a hand streaming plenty of mickeys while every cursor
// position stays inside a tiny box - gentle warping keeps the pointer jiggling around the
// recenter point. Precise desktop work never trips it: a careful hand produces proportionally
// FEW mickeys (ballistics map slow motion ~1:1), nowhere near kBoxRawSum in one window.
// kBoxRawSum accumulates over the (fixed-duration) window, so it is tick-rate independent.
constexpr int kBoxRawSum = 400;  // mickeys accumulated in the window = deliberate sweeping
constexpr int kBoxSpanPx = 30;   // and the cursor went nowhere: confined
// Millisecond baselines for the tick-count thresholds (issue #223). Chosen so the derivation
// at 144Hz reproduces the original field-tuned tick constants EXACTLY (6/3/45/12/24); at other
// refresh rates the detector keeps the same real-time behavior instead of drifting by hz/144.
// The px thresholds above stay fixed: a warp's per-tick displacement is the warp distance
// itself (instantaneous), not a rate.
constexpr int kLockMs       = 42;    // 6 ticks @144
constexpr int kFreeMs       = 21;    // 3 ticks @144
constexpr int kAnchorLossMs = 312;   // 45 ticks @144 (~0.3s)
constexpr int kWarpQuietMs  = 83;    // 12 ticks @144
constexpr int kBoxWindowMs  = 167;   // 24 ticks @144 (~170ms)
inline int ticksFor(int ms, int hz, int floorTicks) {
    const int t = (ms * hz + 500) / 1000;
    return t < floorTicks ? floorTicks : t;
}
}

void LockDetector::setTickRate(int hz) {
    if (hz <= 0) hz = 144;
    lockTicks_       = ticksFor(kLockMs, hz, 3);
    freeTicks_       = ticksFor(kFreeMs, hz, 2);
    anchorLossTicks_ = ticksFor(kAnchorLossMs, hz, 10);
    warpQuietTicks_  = ticksFor(kWarpQuietMs, hz, 4);
    boxWindowTicks_  = ticksFor(kBoxWindowMs, hz, 8);
    // Deliberate-motion floor is mickeys PER TICK: a longer tick aggregates proportionally more
    // HID motion, so the floor scales with tick duration (4 @144Hz ~= 576 mickeys/s).
    rawActive_ = (4 * 144 + hz / 2) / hz;
    if (rawActive_ < 2) rawActive_ = 2;
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
            if (++anchorMissTicks_ > anchorLossTicks_) { haveAnchor_ = false; warpReturns_ = 0; }
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
        if (++boxTicks_ >= boxWindowTicks_) {
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
        if (freeStreak_ >= freeTicks_ && sinceWarp_ > warpQuietTicks_) {
            locked_ = false;
            warpReturns_ = 0;
        }
    } else if (rawMag >= rawActive_) {
        // Mouse moving but OS cursor frozen -> evidence of a lock.
        lockStreak_++; freeStreak_ = 0;
        if (lockStreak_ >= lockTicks_) locked_ = true;
    } else {
        // Idle (no significant input, cursor still): neither streak grows; hold current state.
        lockStreak_ = 0; freeStreak_ = 0;
    }
    return locked_;
}
}
