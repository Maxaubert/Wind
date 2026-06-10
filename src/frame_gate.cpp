#include "frame_gate.h"
#include <cmath>

namespace wind {

bool RectsIntersect(const GateRect& a, const GateRect& b) {
    if (a.right <= a.left || a.bottom <= a.top) return false;
    if (b.right <= b.left || b.bottom <= b.top) return false;
    return a.left < b.right && b.left < a.right &&
           a.top < b.bottom && b.top < a.bottom;
}

bool SnapshotsDiffer(const FrameSnapshot& a, const FrameSnapshot& b) {
    constexpr double kSrcEps    = 1e-3;   // desktop px; * maxLevel 12 = 0.012 screen px
    constexpr double kCursorEps = 0.05;   // screen px
    constexpr double kLevelEps  = 1e-9;   // effectively exact: any ramp step renders
    if (std::fabs(a.level - b.level) > kLevelEps) return true;
    if (std::fabs(a.srcLeft - b.srcLeft) > kSrcEps) return true;
    if (std::fabs(a.srcTop - b.srcTop) > kSrcEps) return true;
    if (std::fabs(a.cursorScreenX - b.cursorScreenX) > kCursorEps) return true;
    if (std::fabs(a.cursorScreenY - b.cursorScreenY) > kCursorEps) return true;
    if (a.cursorVisible != b.cursorVisible) return true;
    if (a.cursorShapeId != b.cursorShapeId) return true;
    if (a.outlineAlpha != b.outlineAlpha) return true;
    return false;
}

bool IsPresentEcho(bool presentedSinceLastFrame, unsigned accumulatedFrames,
                   unsigned dirtyCount, const GateRect& dirty0, const GateRect& overlay) {
    if (!presentedSinceLastFrame) return false;
    if (accumulatedFrames > 1) return false;   // merged composites could hide a real change
    if (dirtyCount != 1) return false;         // a real change elsewhere adds its own rect
    return dirty0.left == overlay.left && dirty0.top == overlay.top &&
           dirty0.right == overlay.right && dirty0.bottom == overlay.bottom;
}

}
