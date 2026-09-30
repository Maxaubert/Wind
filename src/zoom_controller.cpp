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
// Velocity low-pass (2026-08-28). The ramp used to be a velocity SQUARE WAVE -
// full rate the instant the button went down, dead stop the instant it came up - and the stop is
// what read as "harsh, too coarse" in the field, especially while panning at the same time.
// Native Magnifier eases every notch out (~280ms); this is the same idea as a first-order glide:
// the applied rate chases the commanded rate with a ~45ms time constant, so release tapers over
// ~150ms and direction reversals round off instead of snapping. Time-based, so VRR tick-interval
// variation does not change the felt ease (the same law as the mapper's easing).

// Wheel glide time constant: short enough that a notch feels immediate, long enough that it is a
// glide and not a jump (~95% of the way in ~0.25 s).
static constexpr double kWheelTau = 0.08;

void ZoomController::stepTarget(int steps, double step) {
    if (steps == 0 || step <= 0.0) return;
    const double base = target_ > 0.0 ? target_ : level_;
    const double t = base * std::pow(1.0 + step, (double)steps);
    target_ = std::min(maxLevel_, std::max(minLevel_, t));
}

void ZoomController::tick(double dt) {
    if (dir_ != ZoomDir::None) target_ = 0.0;          // a held zoom takes over from the wheel
    if (target_ > 0.0 && dt > 0.0) {
        // Exponential approach in LOG space: equal steps look equal at any zoom.
        const double a = 1.0 - std::exp(-dt / kWheelTau);
        const double lg = std::log(level_) + (std::log(target_) - std::log(level_)) * a;
        level_ = std::exp(lg);
        if (std::abs(std::log(level_ / target_)) < 0.002) { level_ = target_; target_ = 0.0; }
        level_ = std::min(maxLevel_, std::max(minLevel_, level_));
        rate_ = 0.0; heldIn_ = 0.0;
        return;
    }
    // Track continuous zoom-in hold time for the smooth-zoom ease-in; any non-In direction
    // (release or reverse) resets it, so each fresh zoom-in starts slow again.
    if (dir_ == ZoomDir::In && dt > 0.0) heldIn_ += dt;
    else if (dir_ != ZoomDir::In)        heldIn_ = 0.0;
    if (dt <= 0.0) return;

    // The commanded rate, in signed doublings/sec.
    double target = 0.0;
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
        target = inSpeed_ * accelMult * kZoomDoublingsPerSecond;
    } else if (dir_ == ZoomDir::Out) {
        target = -outSpeed_ * kZoomDoublingsPerSecond;  // out never accelerates
    }

    // Glide the applied rate toward the commanded one, then integrate. The glide keeps moving
    // the level briefly AFTER release (target 0, rate decaying), which is the ease-out.
    if (rateTau_ <= 0.0) rate_ = target;                    // ease-out off: the old hard stop
    else rate_ += (target - rate_) * (1.0 - std::exp(-dt / rateTau_));
    if (dir_ == ZoomDir::None && std::abs(rate_) < 0.05) rate_ = 0.0;   // settle, never glide forever
    if (rate_ == 0.0) return;
    level_ *= std::pow(2.0, dt * rate_);
    level_ = std::min(maxLevel_, std::max(minLevel_, level_));
    // Pinned at a bound with the button released: kill the residual glide so the settle write
    // (rampStopped in the transform model) is not deferred by an invisible decaying rate.
    if (dir_ == ZoomDir::None && (level_ >= maxLevel_ || level_ <= minLevel_)) rate_ = 0.0;
}
void ZoomController::reset() { level_ = minLevel_; dir_ = ZoomDir::None; heldIn_ = 0.0; rate_ = 0.0; target_ = 0.0; }
void ZoomController::setLevel(double l) {
    target_ = 0.0;   // a snap (quick zoom, keep-level) cancels any wheel glide
    level_ = std::min(maxLevel_, std::max(minLevel_, l));
}

QuickZoomResult ApplyQuickZoom(double cur, double stored, double def, double maxLevel) {
    constexpr double kEps = 1e-6;
    constexpr double kStoreThreshold = 2.0;        // remember the level being left only if > 200%
    QuickZoomResult r{cur, stored};
    if (cur > 1.0 + kEps) {                         // zoomed -> snap out to 0%
        if (cur > kStoreThreshold) r.newStored = cur;
        r.newLevel = 1.0;
    } else {                                        // at 0% -> snap in
        double target = (stored > 0.0) ? stored : def;
        if (target > maxLevel) target = maxLevel;
        if (target < 1.0)      target = 1.0;
        r.newLevel = target;
    }
    return r;
}
}
