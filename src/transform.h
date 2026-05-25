#pragma once
namespace wind {
struct Offset { int x; int y; };
// center: virtual lens center in screen pixels. level >= 1.0.
// screenW/H: monitor size in pixels. Returns top-left of the magnified source
// region, clamped so the view stays on screen.
Offset ComputeOffset(double centerX, double centerY, double level, int screenW, int screenH);

struct OffsetF { double x; double y; };
// Float (sub-pixel) source-region top-left, clamped on screen. level >= 1.0.
// Used by the own GPU renderer, which can pan sub-pixel (unlike the integer-offset
// Magnification API).
OffsetF ComputeOffsetF(double centerX, double centerY, double level, int screenW, int screenH);
}
