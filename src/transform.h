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
// screenW/H bound the result (issue #148 trigger 2): the mapper clamps the FLOAT source to
// maxX = w - w/level, which is fractional at any mid-ramp level; naive round-to-nearest can push
// the integer offset (or the private translation) past it, so the magnified source rect samples
// OUTSIDE the desktop texture - field-confirmed GPU driver reset (TDR), always at the right or
// bottom edge (left/top clamp to exact 0 and cannot overshoot). Clamp AFTER rounding, downward.
struct MagTransform { int offX; int offY; int txX; int txY; };
MagTransform ComputeMagTransform(double srcLeft, double srcTop, double level,
                                 int screenW, int screenH);

// MagSetInputTransform rects (issue #185; docs/POINTER-HITTEST-FINDINGS.md): pointer-framework
// apps hit-test mouse input through the system input transform under a fullscreen
// magnification, so every transform change must publish src = the magnified source region and
// dst = the monitor rect - BOTH in virtual-screen coordinates (srcLeft/srcTop arrive
// monitor-local; monX/monY are the monitor origin). Rounding nearest; extent = monitor/level.
// At level <= 1.001 the caller disables the transform instead (enabled=false), so no special
// case here: the math is total.
struct InputTransformRects { int sl, st, sr, sb; int dl, dt, dr, db; };
InputTransformRects ComputeInputTransformRects(double srcLeft, double srcTop, double level,
                                               int monX, int monY, int monW, int monH);

// Foreign-writer detection for the system input transform (issue #217). The publish is ONE
// system-wide slot and native Magnifier re-publishes an ENABLED IDENTITY into it continuously
// while it runs - even sitting at 100% - which unmoors the cursor under a Wind zoom (the wobble).
// A dirty Magnifier exit also strands its last rect there. So the model compares what the slot
// ACTUALLY holds against what Wind last published; any difference means a foreign writer (or a
// stale corpse) owns the slot and Wind must republish. Exact integer compare: our own publish
// echoes back verbatim, so any deviation is foreign by definition.
bool InputTransformStomped(bool expectedEnabled, int esl, int est, int esr, int esb,
                           bool actualEnabled, int asl, int ast, int asr, int asb);
}
