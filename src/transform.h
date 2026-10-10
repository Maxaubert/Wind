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
// screenW/H bound the result (issue #148 trigger 2): the mapper clamps the FLOAT source to
// maxX = w - w/level, which is fractional at any mid-ramp level; naive round-to-nearest can push
// the integer offset (or the private translation) past it, so the magnified source rect samples
// OUTSIDE the desktop texture - field-confirmed GPU driver reset (TDR), always at the right or
// bottom edge (left/top clamp to exact 0 and cannot overshoot). Clamp AFTER rounding, downward.
//
// EDGE SAMPLING MARGIN (the left/top edge line). The margin above is one-sided in the code that
// predates it because only the right/bottom can OVERSHOOT - the left/top clamp to exact 0 and
// cannot. But 0 is not a safe source origin either: DWM's NEAREST magnification path resolves
// each destination column to a source texel around a half-texel offset, so at source left EXACTLY
// 0 the leftmost destination columns resolve BELOW column 0, outside the desktop texture, and DWM
// fills them with an undefined light-grey border - a vertical line ~level/2 px wide down the
// screen's left edge (the same along the top), visible only once the view is parked against that
// boundary. Native Magnifier never shows it because it samples SMOOTH by default and that filter
// clamps to edge; our shipped txSamplingMode=0 is nearest, which does not. So the source rect is
// held `edgeMargin` texels inside the texture on the LOW side too. edgeMargin 0 = old behaviour.
struct MagTransform { int offX; int offY; int txX; int txY; };
MagTransform ComputeMagTransform(double srcLeft, double srcTop, double level,
                                 int screenW, int screenH, double edgeMargin = 0.0,
                                 double farMargin = 2.0);

// EDGE MARGINS FOLLOW THE SAMPLING MODE (field report 2026-10-09: zoomed, a click in the
// bottom-left corner did not open Start; native Magnifier opens it at every level). Both margins
// keep desktop texels out of the view, and the pointer-framework hit-test ignores a pointer outside
// the view, so the corner pixel was dead: measured, either margin alone kills it, both at 0 fix it.
// Both margins defend the NEAREST path: the low one hides its grey border line, the far one the
// right/bottom driver reset (#148; the 16-bit MPO field that lives in the nearest path). Smooth
// sampling takes a float path, clamps to edge and survives the same corner, as native Magnifier
// does, so it gets native's exact rect: 0 and 0. Nearest (and an unset mode) keeps both.
struct EdgeMargins { double lo; double hi; };
EdgeMargins EdgeMarginsFor(int samplingMode, double cfgLowMargin);

// The low-side margin that is actually applicable at this level, so every caller that needs the
// source rect (the visual write, the input-transform publish, the weld geometry) derives
// it from ONE formula and they can never describe different rects. Near 1x there is no room for a
// margin at all (the source rect IS the screen), and there the result is 0 - the identity
// transform at rest must stay exactly identity.
double SrcEdgeFloor(double edgeMargin, double level, int screenExtent);

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
