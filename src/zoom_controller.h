#pragma once
namespace wind {
enum class ZoomDir { None, In, Out };

// Pure: given which side buttons are physically held, what should the zoom do.
// Both held is ambiguous, so freeze.
ZoomDir ResolveDirection(bool inHeld, bool outHeld);

class ZoomController {
public:
    ZoomController(double minLevel, double maxLevel);
    void setDirection(ZoomDir d);
    ZoomDir direction() const { return dir_; }
    // Speed/acceleration profile (hot-reloadable; does NOT reset the level):
    //   inSpeed/outSpeed - per-direction rate multipliers (1.0 = base; both modes)
    //   smooth           - soft-start the zoom-IN while held (ease in up to the linear rate)
    //   accel            - smooth ease-in depth: in-rate starts at inSpeed/accel and climbs to
    //                      inSpeed (the linear cap, never exceeded); >1 (<=1 = no ease-in)
    //   rampSeconds      - seconds of continuous zoom-in to reach the linear rate (<=0 = instant)
    void setProfile(double inSpeed, double outSpeed, bool smooth, double accel, double rampSeconds);
    // Release ease-out time constant, seconds (0 = the old dead stop). Hot like setProfile.
    void setEaseOut(double tauSeconds) { rateTau_ = tauSeconds > 0.0 ? tauSeconds : 0.0; }
    void tick(double dtSeconds);   // ramp level multiplicatively toward bound
    double level() const { return level_; }
    void reset();                  // level=min, dir=None, held cleared
    void setLevel(double l);       // instant snap to a level (clamped to [min,max]); dir_ untouched
    // Scroll-wheel zoom (#285): move a TARGET level by x(1+step) per step (negative = out), clamped;
    // tick() glides the level to it. Steps stack on the target, so fast scrolling reads as one
    // continuous zoom. A held zoom direction takes over at once (the target is dropped).
    void stepTarget(int steps, double step);
    bool hasTarget() const { return target_ > 0.0; }
private:
    double minLevel_, maxLevel_;
    double level_;
    double target_ = 0.0;                      // wheel target level (0 = none)
    ZoomDir dir_ = ZoomDir::None;
    double inSpeed_ = 1.0, outSpeed_ = 1.0;   // defaults reproduce today's behavior
    bool   smooth_ = false;
    double accel_ = 3.0, rampSeconds_ = 0.6;
    double heldIn_ = 0.0;                      // continuous seconds zoom-in held (drives accel ramp)
    double rateTau_ = 0.045;                   // glide time constant (setEaseOut; 0 = dead stop)
    double rate_ = 0.0;                        // applied rate (signed doublings/s) - glides toward
                                               // the commanded rate so release EASES OUT instead of
                                               // stopping dead (the square-wave stop read as harsh)
};

// Result of one quick-zoom toggle: the level to snap to, and the (possibly updated) remembered level.
struct QuickZoomResult { double newLevel; double newStored; };
// Pure toggle arithmetic. cur = current level, stored = remembered level (0 = none yet), def = the
// configured default, maxLevel = ceiling. If zoomed (cur > 1.0): snap out to 1.0, remembering cur
// only when it is above 200% (cur > 2.0). If at 1.0: snap in to stored (or def if none), clamped to
// [1.0, maxLevel].
QuickZoomResult ApplyQuickZoom(double cur, double stored, double def, double maxLevel);
}
