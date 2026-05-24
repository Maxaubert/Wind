#pragma once
namespace wind {
struct Offset { int x; int y; };
// center: virtual lens center in screen pixels. level >= 1.0.
// screenW/H: monitor size in pixels. Returns top-left of the magnified source
// region, clamped so the view stays on screen.
Offset ComputeOffset(double centerX, double centerY, double level, int screenW, int screenH);
}
