#include "tracker.h"
#include <algorithm>
namespace wind {
Tracker::Tracker(int screenW, int screenH, double sensitivity)
    : screenW_(screenW), screenH_(screenH), sensitivity_(sensitivity),
      cx_(screenW / 2.0), cy_(screenH / 2.0),
      lastCursorX_(0), lastCursorY_(0) {}

void Tracker::clamp() {
    cx_ = std::min(std::max(cx_, 0.0), static_cast<double>(screenW_));
    cy_ = std::min(std::max(cy_, 0.0), static_cast<double>(screenH_));
}

void Tracker::update(int cursorX, int cursorY, int rawDx, int rawDy) {
    bool cursorMoved = !haveCursor_ || cursorX != lastCursorX_ || cursorY != lastCursorY_;
    bool rawMoved = (rawDx != 0 || rawDy != 0);

    if (cursorMoved) {
        // OS cursor moving freely: follow it and abandon any lock at once.
        locked_ = false;
        stationaryRawTicks_ = 0;
        cx_ = cursorX;
        cy_ = cursorY;
    } else if (rawMoved) {
        // OS cursor frozen but the hand is moving: candidate for a cursor lock.
        if (!locked_ && ++stationaryRawTicks_ >= kLockEngageTicks)
            locked_ = true;
        if (locked_) {
            cx_ += rawDx * sensitivity_;   // pan the lens with raw movement
            cy_ += rawDy * sensitivity_;
        }
        // While not yet locked: hold the centre (do NOT jump). Removes the flicker.
    } else {
        // Neither cursor nor hand moved: hold. Reset the engage counter so only
        // *consecutive* frozen+raw ticks arm a lock; an established lock_ stays on
        // (a locked game cursor remains frozen when you briefly stop moving).
        stationaryRawTicks_ = 0;
    }
    clamp();
    lastCursorX_ = cursorX;
    lastCursorY_ = cursorY;
    haveCursor_ = true;
}

void Tracker::recenter() {
    cx_ = screenW_ / 2.0;
    cy_ = screenH_ / 2.0;
}
}
