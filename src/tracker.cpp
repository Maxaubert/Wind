#include "tracker.h"
#include <algorithm>
namespace wind {
Tracker::Tracker(int screenW, int screenH, double sensitivity, double centerDeadzone)
    : screenW_(screenW), screenH_(screenH), sensitivity_(sensitivity),
      deadzoneFrac_(centerDeadzone),
      cx_(screenW / 2.0), cy_(screenH / 2.0),
      lastCursorX_(0), lastCursorY_(0) {}

void Tracker::clamp() {
    cx_ = std::min(std::max(cx_, 0.0), static_cast<double>(screenW_));
    cy_ = std::min(std::max(cy_, 0.0), static_cast<double>(screenH_));
}

void Tracker::followWithDeadzone(int cursorX, int cursorY, double level) {
    // The cursor reaches the view edge at |cursor - center| = (screen/level)/2. Let it
    // glide freely within deadzoneFrac of that band before the view pans; once it pushes
    // past, the view follows just enough to hold the cursor at the band edge. While the
    // cursor stays inside the band the center (and thus the integer offset) does not
    // move, so the cursor glides smoothly instead of being re-snapped/re-rounded to
    // center every frame (the high-zoom jitter).
    double hdX = (screenW_ / level) * 0.5 * deadzoneFrac_;
    double hdY = (screenH_ / level) * 0.5 * deadzoneFrac_;
    if (cursorX - cx_ >  hdX)      cx_ = cursorX - hdX;
    else if (cx_ - cursorX >  hdX) cx_ = cursorX + hdX;
    if (cursorY - cy_ >  hdY)      cy_ = cursorY - hdY;
    else if (cy_ - cursorY >  hdY) cy_ = cursorY + hdY;
}

void Tracker::update(int cursorX, int cursorY, int rawDx, int rawDy, double level) {
    bool cursorMoved = !haveCursor_ || cursorX != lastCursorX_ || cursorY != lastCursorY_;
    bool rawMoved = (rawDx != 0 || rawDy != 0);

    if (cursorMoved) {
        // OS cursor moving freely: follow it and abandon any lock at once.
        locked_ = false;
        stationaryRawTicks_ = 0;
        if (level > 1.0 && deadzoneFrac_ > 0.0)
            followWithDeadzone(cursorX, cursorY, level);   // glide within a central band
        else { cx_ = cursorX; cy_ = cursorY; }             // rigid center / not zoomed
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
