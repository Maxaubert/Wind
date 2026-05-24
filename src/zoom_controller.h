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
    void tick(double dtSeconds);   // ramp level multiplicatively toward bound
    double level() const { return level_; }
    void reset();                  // level=min, dir=None
private:
    double minLevel_, maxLevel_, fullRangeSeconds_;
    double level_;
    ZoomDir dir_ = ZoomDir::None;
};
}
