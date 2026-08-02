# Drag flicker: the weld fights the hand (issue #169) - design

## Problem

Dragging a window while zoomed makes it flicker between two positions ~85 px apart (probe-measured,
square-wave alternation at sample rate). The window's position tracks the pointer 1:1 at full rate,
so nothing is slow - the OS cursor itself oscillates. Amplitude grows with hand speed (and with the
radius of arc-shaped hand motion, which is speed in disguise). Present in render and transform;
absent at 1x, absent with Wind quit, absent in the native Windows Magnifier.

## Root cause

Two defects in the weld/baseline contract, one structural and one behavioural.

### 1. The motion baseline is assumed, not measured

The free-pan oracle computes each tick's hand delta as `GetCursorPos() - lastSetVirtual`, and
`lastSetVirtual` is set at end of tick to `clickDesktop` - the point the render model was ASKED to
park the pointer at (main.cpp, "Both models now WELD ... the baseline for next tick's delta is that
point"). That assumption is false in every case where no park landed:

- The transform model NEVER places the cursor (the transform cursor law), yet its baseline is still
  set to the lens centre.
- The render model's park is deduped: it fires only when `clickDesktop` changed a whole pixel
  (render_engine.cpp `lastClickX` check), so slow motion skips it while the pointer creeps on.
- A park can also be skipped by early-outs (device lost, parked overlay) or simply fail.

When the park does not land, the pointer's true position differs from the assumed baseline by some
gap `e`. The next delta then measures `hand + e` and the mapper integrates it: the centre advances
by MORE than the hand, overshoots the pointer, `e` flips sign, and the loop oscillates - an
unstable servo. Amplitude scales with speed and with the smoothing lag, matching the measurements
(and turning smoothing OFF removed damping, making it worse: 537 jumps vs 136).

### 2. During a drag, the weld itself is the fight

While a mouse button is held, the OS pointer IS the drag position. The render model re-parks it at
the smoothed lens centre every tick while the hand advances it, so the pointer alternates between
the two and everything that follows the pointer (the dragged window) flickers. This is inherent:
any per-tick repositioning of a pointer that another interaction is actively consuming is a fight.

## Fix

Three layers, each independently sound.

### A. Measured baseline

After the model presents, read `GetCursorPos()` once and use THAT as `lastSetVirtual`. If the park
landed, this equals the park point (SetCursorPos is synchronous) - identical to today. If no park
landed (transform follow, dedupe skip, failure), it is the pointer's true position and the servo
stays consistent. The sub-tick motion between park and read is counted in the next delta; nothing
is lost. Inspect keeps its explicit frozen-point baseline (the 1px clip pins the pointer; the
measured read would return the same, but explicit is clearer and immune to the click-release
window). The freeze branch likewise keeps `freezePoint` semantics via the same measured read (the
clip confines the pointer, so the read returns the frozen point).

### B. Drag-follow: suppress the weld while a button is held

In a FREE (not locked, not gameFreeze, not Inspect) render-model session, when any physical mouse
button is down (`GetAsyncKeyState` on VK_L/R/MBUTTON):

- Do not park: a new `RenderFrameParams::suppressCursorSync` flag makes renderFrame skip its
  SetCursorPos block.
- Pan by the pointer's real motion UNSCALED (`dx = curDx`, the transform FOLLOW design), so the
  lens tracks the pointer 1:1. `cursorSensitivity` deliberately does not apply while a button is
  held - scaling would desync the lens from the pointer that owns the drag.

Correctness: the press happened under the welded cursor (weld was live until the button went down),
so it landed at the right point. During the drag the pointer position is the drag position; the
lens follows it, and the drawn centred cursor sits over the same content modulo the smoothing lag
(~0.67 x per-tick speed at the shipped 0.4, settling to zero when the hand slows to drop). The
release lands where the pointer and the window both are - correct by construction. On release the
weld resumes and re-parks the pointer to the lens centre (a few px at most).

The decision is a pure function, `ShouldDragFollow(renderActive, locked, gameFreeze, inspect,
anyButtonDown)` in a new small header, unit-tested as a truth table.

### C. Divergence diagnostics (off by default)

Under `diagnostics=1`, log a once-per-second aggregate while zoomed: parks issued, parks skipped,
max |pointer - centre| divergence. If any oscillation survives, the log pinpoints it.

## Out of scope

- The transform model's own drag behaviour is already FOLLOW (never places the cursor); it gains
  the measured baseline (fixing its servo) and needs no drag special-case.
- Inspect, game freeze, locked sessions: unchanged.
- The magnify-mode flicker report: Wind touches nothing cursor-related there; needs re-verification
  after this fix before it is treated as real (noted on #169).

## Testing

- New truth-table unit tests for `ShouldDragFollow`; the 145 existing tests must pass.
- Field: drag windows zoomed (slow, fast, near and far from screen centre), text-selection drags,
  click accuracy after a drag, transform-model drag, a game session sanity check.
