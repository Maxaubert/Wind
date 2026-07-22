#pragma once
namespace wind {
struct OffsetF { double x; double y; };
// Float (sub-pixel) source-region top-left, clamped on screen. level >= 1.0.
// center: virtual lens center in screen pixels; screenW/H: monitor size in pixels.
// Used by the own GPU renderer, which pans sub-pixel.
OffsetF ComputeOffsetF(double centerX, double centerY, double level, int screenW, int screenH);
}
