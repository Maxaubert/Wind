#include "cursor_mapper.h"
#include "transform.h"
namespace wind {
CursorMapper::CursorMapper(int screenW, int screenH, double sensitivity)
    : sw_(screenW), sh_(screenH), sens_(sensitivity),
      cx_(screenW / 2.0), cy_(screenH / 2.0) {}

void CursorMapper::reset(double centerX, double centerY) { cx_ = centerX; cy_ = centerY; }

MapResult CursorMapper::update(int rawDx, int rawDy, double level) {
    if (level < 1.0) level = 1.0;
    // Sub-pixel lens integration at *desktop* speed (not divided by zoom): the focus moves
    // at the same hand-speed whether you're at 2x or 8x, matching Windows Magnifier. The
    // world therefore scrolls faster on screen at higher zoom, which is the natural feel;
    // dividing by level (an earlier try) made high zoom feel sluggish. Tune with sensitivity.
    cx_ += rawDx * sens_;
    cy_ += rawDy * sens_;
    if (cx_ < 0) cx_ = 0; else if (cx_ > sw_) cx_ = sw_;
    if (cy_ < 0) cy_ = 0; else if (cy_ > sh_) cy_ = sh_;

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
