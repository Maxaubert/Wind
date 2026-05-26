#pragma once
namespace wind {
enum class ZoomDir { None, In, Out };

// Pure: given which side buttons are physically held, what should the zoom do.
// Both held is ambiguous, so freeze.
ZoomDir ResolveDirection(bool inHeld, bool outHeld);

class ZoomController {
public:
    ZoomController(double minLevel, double maxLevel, double fullRangeSeconds);
    void setDirection(ZoomDir d);
    ZoomDir direction() const { return dir_; }
    // Speed/acceleration profile (hot-reloadable; does NOT reset the level):
    //   inSpeed/outSpeed - per-direction rate multipliers (1.0 = base; both modes)
    //   smooth           - accelerate zoom-IN while held
    //   accel            - smooth-mode top in-rate = inSpeed * accel (clamped >=1; 1 = no accel)
    //   rampSeconds      - seconds of continuous zoom-in to reach the top in-rate (<=0 = instant)
    void setProfile(double inSpeed, double outSpeed, bool smooth, double accel, double rampSeconds);
    void tick(double dtSeconds);   // ramp level multiplicatively toward bound
    double level() const { return level_; }
    void reset();                  // level=min, dir=None, held cleared
private:
    double minLevel_, maxLevel_, fullRangeSeconds_;
    double level_;
    ZoomDir dir_ = ZoomDir::None;
    double inSpeed_ = 1.0, outSpeed_ = 1.0;   // defaults reproduce today's behavior
    bool   smooth_ = false;
    double accel_ = 3.0, rampSeconds_ = 0.6;
    double heldIn_ = 0.0;                      // continuous seconds zoom-in held (drives accel ramp)
};
}
