#include "zoom_controller.h"
#include <algorithm>
#include <cmath>
namespace wind {
ZoomDir ResolveDirection(bool inHeld, bool outHeld) {
    if (inHeld == outHeld) return ZoomDir::None; // neither, or both
    return inHeld ? ZoomDir::In : ZoomDir::Out;
}
ZoomController::ZoomController(double minLevel, double maxLevel, double fullRangeSeconds)
    : minLevel_(minLevel), maxLevel_(maxLevel),
      fullRangeSeconds_(fullRangeSeconds), level_(minLevel) {}
void ZoomController::setDirection(ZoomDir d) { dir_ = d; }
void ZoomController::setProfile(double inSpeed, double outSpeed, bool smooth,
                                double accel, double rampSeconds) {
    inSpeed_ = inSpeed; outSpeed_ = outSpeed; smooth_ = smooth;
    accel_ = accel; rampSeconds_ = rampSeconds;
}
void ZoomController::tick(double dt) {
    // Track continuous zoom-in hold time for the smooth-zoom accel ramp; any non-In direction
    // (release or reverse) resets it, so each fresh zoom-in starts slow again.
    if (dir_ == ZoomDir::In && dt > 0.0) heldIn_ += dt;
    else if (dir_ != ZoomDir::In)        heldIn_ = 0.0;
    if (dir_ == ZoomDir::None || dt <= 0.0) return;

    double speed;
    if (dir_ == ZoomDir::In) {
        double accelMult = 1.0;
        if (smooth_ && accel_ > 1.0) {                 // accel<=1 -> no acceleration
            double t = (rampSeconds_ > 0.0 && heldIn_ < rampSeconds_)
                         ? heldIn_ / rampSeconds_       // 0..1 ramp (ramp<=0 -> instant top)
                         : 1.0;
            accelMult = 1.0 + (accel_ - 1.0) * t;       // 1..accel
        }
        speed = inSpeed_ * accelMult;
    } else {
        speed = outSpeed_;                              // out never accelerates
    }
    double f = std::pow(maxLevel_ / minLevel_, dt * speed / fullRangeSeconds_);
    if (dir_ == ZoomDir::In)  level_ *= f;
    else                      level_ /= f;
    level_ = std::min(maxLevel_, std::max(minLevel_, level_));
}
void ZoomController::reset() { level_ = minLevel_; dir_ = ZoomDir::None; heldIn_ = 0.0; }
}
