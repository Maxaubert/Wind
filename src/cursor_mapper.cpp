#include "cursor_mapper.h"
#include "transform.h"
#include <cmath>
namespace wind {
CursorMapper::CursorMapper(int screenW, int screenH, double smoothing)
    : sw_(screenW), sh_(screenH),
      cx_(screenW / 2.0), cy_(screenH / 2.0), tx_(screenW / 2.0), ty_(screenH / 2.0) {
    alpha_ = 1.0 - smoothing;
    if (alpha_ > 1.0) alpha_ = 1.0;
    if (alpha_ < 0.05) alpha_ = 0.05;     // never fully stall (keep responsiveness)
}

bool CursorMapper::settled() const {
    // 0.5 px: once the rendered center is within half a pixel of the target, further easing is
    // sub-pixel and produces no visible movement, so the view can stop redrawing.
    return std::abs(cx_ - tx_) < 0.5 && std::abs(cy_ - ty_) < 0.5;
}

void CursorMapper::reset(double centerX, double centerY) {
    cx_ = tx_ = centerX; cy_ = ty_ = centerY;
}

MapResult CursorMapper::update(int dx, int dy, double level) {
    if (level < 1.0) level = 1.0;
    // Apply the caller-resolved pixel delta at *desktop* speed (not divided by zoom): the focus
    // reaches things at the same hand-speed whether at 2x or 8x, matching Windows Magnifier.
    tx_ += dx;
    ty_ += dy;
    if (tx_ < 0) tx_ = 0; else if (tx_ > sw_) tx_ = sw_;
    if (ty_ < 0) ty_ = 0; else if (ty_ > sh_) ty_ = sh_;

    // Light inertia: ease the rendered center toward the target. Smooths jerk and the uneven
    // per-frame delta steps; alpha_ = 1 means no smoothing (snaps to target).
    cx_ += (tx_ - cx_) * alpha_;
    cy_ += (ty_ - cy_) * alpha_;

    OffsetF o = ComputeOffsetF(cx_, cy_, level, sw_, sh_);
    MapResult r;
    r.srcLeft = o.x; r.srcTop = o.y;
    // Center normally (the lens centers on cx_,cy_), shifting toward an edge when the
    // source rect clamps against the desktop boundary so corners stay reachable.
    r.cursorScreenX = (cx_ - o.x) * level;
    r.cursorScreenY = (cy_ - o.y) * level;
    r.clickDesktopX = static_cast<int>(cx_ + 0.5);
    r.clickDesktopY = static_cast<int>(cy_ + 0.5);
    return r;
}
}
