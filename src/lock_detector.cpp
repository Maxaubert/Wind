#include "lock_detector.h"
namespace wind {
namespace {
constexpr int kRawActive  = 4;   // raw magnitude that counts as deliberate mouse motion
constexpr int kCursorMoved = 1;  // OS cursor moved at least this many px (it tracked input)
constexpr int kLockTicks  = 6;   // consecutive raw-active + cursor-frozen ticks -> lock
constexpr int kFreeTicks  = 3;   // consecutive cursor-moving ticks -> unlock
}

void LockDetector::reset() { locked_ = false; lockStreak_ = 0; freeStreak_ = 0; }

bool LockDetector::update(bool clipConfined, int rawMag, int cursorMag) {
    // Direct, reliable signal: a confined clip rect means a game has clipped the cursor.
    if (clipConfined) { locked_ = true; lockStreak_ = 0; freeStreak_ = 0; return locked_; }

    if (cursorMag >= kCursorMoved) {
        // The OS cursor is tracking input -> evidence of free movement.
        freeStreak_++; lockStreak_ = 0;
        if (freeStreak_ >= kFreeTicks) locked_ = false;
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
