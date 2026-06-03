#include "zoom_controller.h"
#include <algorithm>
#include <cmath>
namespace wind {
ZoomDir ResolveDirection(bool inHeld, bool outHeld) {
    if (inHeld == outHeld) return ZoomDir::None; // neither, or both
    return inHeld ? ZoomDir::In : ZoomDir::Out;
}
// Base zoom rate, INDEPENDENT of maxLevel: at speed 1.0 the magnification doubles this many times
// per second, so maxLevel only sets how FAR you can zoom, not how fast. 2.5/s reaches 8x in ~1.2s
// at speed 1.0 (matches the original default feel); zoomInSpeed/zoomOutSpeed scale it.
static constexpr double kZoomDoublingsPerSecond = 2.5;
ZoomController::ZoomController(double minLevel, double maxLevel)
    : minLevel_(minLevel), maxLevel_(maxLevel), level_(minLevel) {}
void ZoomController::setDirection(ZoomDir d) { dir_ = d; }
void ZoomController::setProfile(double inSpeed, double outSpeed, bool smooth,
                                double accel, double rampSeconds) {
    inSpeed_ = inSpeed; outSpeed_ = outSpeed; smooth_ = smooth;
    accel_ = accel; rampSeconds_ = rampSeconds;
}
void ZoomController::tick(double dt) {
    // Track continuous zoom-in hold time for the smooth-zoom ease-in; any non-In direction
    // (release or reverse) resets it, so each fresh zoom-in starts slow again.
    if (dir_ == ZoomDir::In && dt > 0.0) heldIn_ += dt;
    else if (dir_ != ZoomDir::In)        heldIn_ = 0.0;
    if (dir_ == ZoomDir::None || dt <= 0.0) return;

    double speed;
    if (dir_ == ZoomDir::In) {
        // Smooth zoom is a soft start: the in-rate climbs from a slow start up to the LINEAR rate
        // (inSpeed) and never exceeds it. accelMult ramps from 1/accel to 1 over rampSeconds.
        double accelMult = 1.0;
        if (smooth_ && accel_ > 1.0) {                 // accel<=1 -> no ease-in (pure linear)
            double t = (rampSeconds_ > 0.0 && heldIn_ < rampSeconds_)
                         ? heldIn_ / rampSeconds_       // 0..1 ramp (ramp<=0 -> instant linear)
                         : 1.0;
            double startFrac = 1.0 / accel_;            // slow start = linear / accel
            accelMult = startFrac + (1.0 - startFrac) * t;  // startFrac..1 (caps AT linear, never above)
        }
        speed = inSpeed_ * accelMult;
    } else {
        speed = outSpeed_;                              // out never accelerates
    }
    double f = std::pow(2.0, dt * speed * kZoomDoublingsPerSecond);
    if (dir_ == ZoomDir::In)  level_ *= f;
    else                      level_ /= f;
    level_ = std::min(maxLevel_, std::max(minLevel_, level_));
}
void ZoomController::reset() { level_ = minLevel_; dir_ = ZoomDir::None; heldIn_ = 0.0; }
void ZoomController::setLevel(double l) {
    level_ = std::min(maxLevel_, std::max(minLevel_, l));
}
}
