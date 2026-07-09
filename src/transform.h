#pragma once
namespace wind {
struct OffsetF { double x; double y; };
// Float (sub-pixel) source-region top-left, clamped on screen. level >= 1.0.
// center: virtual lens center in screen pixels; screenW/H: monitor size in pixels.
// Used by the own GPU renderer, which pans sub-pixel.
OffsetF ComputeOffsetF(double centerX, double centerY, double level, int screenW, int screenH);

// Source top-left that makes `center` the FIXED POINT of the fullscreen transform: T(center)==center.
//
// The render model draws the cursor into its own frame, so it can centre the view on the cursor. The
// transform model cannot: DWM composites the cursor AND layered windows OUTSIDE the fullscreen
// magnification (measured - a layered window at desktop P stays at screen P, unscaled, while the
// content magnifies around it). So whatever draws the cursor lands at the unmagnified point L, while
// the item a click at L hits is drawn at T(L). Centring makes T(L) the screen centre, so the two
// diverge by (centre - L): zero at the centre, growing toward the edges. That is the click drift.
//
// Anchoring the transform at the cursor instead makes the content under the cursor be the content AT
// the cursor, so an unmagnified cursor always sits on exactly what it clicks - regardless of what DWM
// does or does not magnify. The result always lies in [0, screen - screen/level], so unlike the
// centred offset it NEVER clamps, which is what removes the dead zones at the screen edges.
OffsetF ComputeFixedPointOffset(double centerX, double centerY, double level);

// The two forms of the fullscreen-magnifier transform, both derived from the sub-pixel source
// top-left (srcLeft/srcTop, screen px) the CursorMapper already clamps. off* is the whole-pixel
// offset the public MagSetFullscreenTransform takes; tx* is the screen-space translation
// (-source * level) the private SetMagnificationDesktopMagnification channel takes, which pans
// level-times more finely so slow sub-pixel drift still moves ~1px/frame instead of stalling.
struct MagTransform { int offX; int offY; int txX; int txY; };
MagTransform ComputeMagTransform(double srcLeft, double srcTop, double level);
}
