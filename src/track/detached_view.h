#pragma once
// A frame whose view is NOT centred on the pointer (issue #276): caret/focus tracking and, later,
// mouse edge mode. The view comes from (viewCx, viewCy); the cursor fields report the REAL pointer,
// so the sprite / drawn cursor sit where the pointer actually is and scroll out of view with the
// content. Pure; tests/test_detached_view.cpp.
#include "cursor_mapper.h"
#include "transform.h"
namespace wind {
inline MapResult DetachedMap(double viewCx, double viewCy, double ptrX, double ptrY, double level,
                             int monW, int monH) {
    if (level < 1.0) level = 1.0;
    const OffsetF o = ComputeOffsetF(viewCx, viewCy, level, monW, monH);
    MapResult r;
    r.srcLeft = o.x; r.srcTop = o.y;
    r.centerX = viewCx; r.centerY = viewCy;
    r.cursorScreenX = (ptrX - o.x) * level;
    r.cursorScreenY = (ptrY - o.y) * level;
    r.clickDesktopX = static_cast<int>(ptrX + (ptrX >= 0 ? 0.5 : -0.5));
    r.clickDesktopY = static_cast<int>(ptrY + (ptrY >= 0 ? 0.5 : -0.5));
    return r;
}
}  // namespace wind
