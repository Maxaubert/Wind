#pragma once
namespace wind {
struct OffsetF { double x; double y; };
// Float (sub-pixel) source-region top-left, clamped on screen. level >= 1.0.
// center: virtual lens center in screen pixels; screenW/H: monitor size in pixels.
// Used by the own GPU renderer, which pans sub-pixel.
OffsetF ComputeOffsetF(double centerX, double centerY, double level, int screenW, int screenH);

// The two forms of the fullscreen-magnifier transform, both derived from the sub-pixel source
// top-left (srcLeft/srcTop, screen px) the CursorMapper already clamps. off* is the whole-pixel
// offset the public MagSetFullscreenTransform takes; tx* is the screen-space translation
// (-source * level) the private SetMagnificationDesktopMagnification channel takes, which pans
// level-times more finely so slow sub-pixel drift still moves ~1px/frame instead of stalling.
struct MagTransform { int offX; int offY; int txX; int txY; };
MagTransform ComputeMagTransform(double srcLeft, double srcTop, double level);
}
