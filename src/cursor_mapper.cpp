#include "cursor_mapper.h"
#include "transform.h"
#include <cmath>
namespace wind {
CursorMapper::CursorMapper(int screenW, int screenH)
    : sw_(screenW), sh_(screenH), cx_(screenW / 2.0), cy_(screenH / 2.0) {}

void CursorMapper::reset(double centerX, double centerY) {
    cx_ = centerX; cy_ = centerY;
}

MapResult CursorMapper::update(int dx, int dy, double level) {
    if (level < 1.0) level = 1.0;
    // Apply the caller-resolved pixel delta at *desktop* speed (not divided by zoom): the focus
    // reaches things at the same hand-speed whether at 2x or 8x, matching Windows Magnifier.
    cx_ += dx;
    cy_ += dy;
    if (cx_ < 0) cx_ = 0; else if (cx_ > sw_) cx_ = sw_;
    if (cy_ < 0) cy_ = 0; else if (cy_ > sh_) cy_ = sh_;
    // Pan wall (issue #148): keep the SOURCE left edge at or under maxSrcX_ by bounding the
    // center. During a zoom ramp at the right edge the wall moves inward with the level, and the
    // center follows it the same tick.
    if (maxSrcX_ >= 0.0) {
        double centerMax = maxSrcX_ + (sw_ / level) / 2.0;
        if (centerMax > sw_) centerMax = sw_;
        if (cx_ > centerMax) cx_ = centerMax;
    }
    // Y wall (issue #191): identical shape - the 16-bit wrap is per-axis and Y was unguarded.
    if (maxSrcY_ >= 0.0) {
        double centerMaxY = maxSrcY_ + (sh_ / level) / 2.0;
        if (centerMaxY > sh_) centerMaxY = sh_;
        if (cy_ > centerMaxY) cy_ = centerMaxY;
    }

    OffsetF o = ComputeOffsetF(cx_, cy_, level, sw_, sh_);
    MapResult r;
    r.srcLeft = o.x; r.srcTop = o.y;
    // Center normally (the lens centers on cx_,cy_), shifting toward an edge when the
    // source rect clamps against the desktop boundary so corners stay reachable.
    r.cursorScreenX = (cx_ - o.x) * level;
    r.cursorScreenY = (cy_ - o.y) * level;
    r.clickDesktopX = static_cast<int>(cx_ + 0.5);
    r.clickDesktopY = static_cast<int>(cy_ + 0.5);
    r.centerX = cx_;   // un-rounded, for the transform model's fixed-point anchor
    r.centerY = cy_;
    return r;
}
}
