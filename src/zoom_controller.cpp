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
void ZoomController::tick(double dt) {
    if (dir_ == ZoomDir::None || dt <= 0.0) return;
    double f = std::pow(maxLevel_ / minLevel_, dt / fullRangeSeconds_);
    if (dir_ == ZoomDir::In)  level_ *= f;
    else                      level_ /= f;
    level_ = std::min(maxLevel_, std::max(minLevel_, level_));
}
void ZoomController::reset() { level_ = minLevel_; dir_ = ZoomDir::None; }
}
