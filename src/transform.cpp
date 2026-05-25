#include "transform.h"
#include <algorithm>
#include <cmath>
namespace wind {
Offset ComputeOffset(double centerX, double centerY, double level, int screenW, int screenH) {
    if (level < 1.0) level = 1.0;
    double viewW = screenW / level;
    double viewH = screenH / level;
    int x = static_cast<int>(std::lround(centerX - viewW / 2.0));
    int y = static_cast<int>(std::lround(centerY - viewH / 2.0));
    int maxX = screenW - static_cast<int>(std::lround(viewW));
    int maxY = screenH - static_cast<int>(std::lround(viewH));
    x = std::min(std::max(x, 0), std::max(maxX, 0));
    y = std::min(std::max(y, 0), std::max(maxY, 0));
    return Offset{ x, y };
}
OffsetF ComputeOffsetF(double centerX, double centerY, double level, int screenW, int screenH) {
    if (level < 1.0) level = 1.0;
    double viewW = screenW / level;
    double viewH = screenH / level;
    double x = centerX - viewW / 2.0;
    double y = centerY - viewH / 2.0;
    double maxX = screenW - viewW;
    double maxY = screenH - viewH;
    if (maxX < 0) maxX = 0;
    if (maxY < 0) maxY = 0;
    if (x < 0) x = 0; else if (x > maxX) x = maxX;
    if (y < 0) y = 0; else if (y > maxY) y = maxY;
    return OffsetF{ x, y };
}
}
