#pragma once
namespace wind {

// Pure frame-skip support for the render engine (no <windows.h> - unit-tested). The engine
// renders only when something that affects the output image changed; otherwise it skips
// draw+present entirely, so DWM has nothing to recomposite and idle-zoomed GPU drops to ~0
// (the same render-on-change behavior that keeps Windows Magnifier at ~1%).

// Plain rect, same layout/meaning as Win32 RECT (fields: left, top, right, bottom).
// Half-open: x in [left, right), y in [top, bottom). Empty/inverted rects never intersect.
struct GateRect { long left, top, right, bottom; };

// True if a and b overlap.
bool RectsIntersect(const GateRect& a, const GateRect& b);

// Everything that affects the rendered image for one tick, reduced to comparable values.
// cursorShapeId is the HCURSOR value (opaque id; changes on shape swap). outlineAlpha is the
// EFFECTIVE alpha: 0 when the outline is disabled or not drawn at this level. Visual config
// knobs (bilinear, sharpness, ...) are deliberately NOT here; a config hot-reload forces one
// render instead (RunTick sets forceRender).
struct FrameSnapshot {
    double level = 0.0;
    double srcLeft = 0.0, srcTop = 0.0;
    double cursorScreenX = 0.0, cursorScreenY = 0.0;
    bool   cursorVisible = false;
    unsigned long long cursorShapeId = 0;
    float  outlineAlpha = 0.0f;
};

// True when rendering b would produce a visibly different image than already-presented a.
// Epsilons (named in the .cpp, covered by tests): source rect 1e-3 desktop px (at zoom 12
// that is 0.012 screen px - invisible, and it lets the cursor-smoothing tail settle instead
// of rendering forever); cursor 0.05 screen px; level 1e-9 (a ramp must always render).
bool SnapshotsDiffer(const FrameSnapshot& a, const FrameSnapshot& b);

// True when an acquired duplication frame is solely the DWM echo of our OWN previous Present.
// The overlay is capture-EXCLUDED (its pixels never appear in the captured image), but DWM
// still reports the overlay's window region as dirty in the next duplication frame after each
// of our presents. Treating that echo as a desktop change chains present -> dirty -> present
// forever, so the idle frame-skip gate never engages (it only broke when a tick's acquire
// happened to race the composite). Signature of the echo, all required: we presented since the
// last acquired image frame, exactly one accumulated composite (>1 may hide a real change
// merged in), and exactly ONE dirty rect exactly equal to the overlay rect (a real change in
// another window adds its own rect, and a partial change differs from the full overlay rect).
// `dirty0` is the first dirty rect; only inspected when dirtyCount == 1.
bool IsPresentEcho(bool presentedSinceLastFrame, unsigned accumulatedFrames,
                   unsigned dirtyCount, const GateRect& dirty0, const GateRect& overlay);

}
