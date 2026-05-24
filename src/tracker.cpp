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
    if (cursorMoved) {
        cx_ = cursorX;            // free mode: follow OS cursor
        cy_ = cursorY;
    } else if (rawDx != 0 || rawDy != 0) {
        cx_ += rawDx * sensitivity_;   // locked mode: integrate raw movement
        cy_ += rawDy * sensitivity_;
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
