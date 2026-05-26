# Wind - Auto-Match Cursor Sensitivity (Design Spec)

**Date:** 2026-05-26
**Status:** Approved for planning
**Branch:** `feat/auto-sensitivity` off `feat/own-renderer`
**Issue:** #38

## Goal

Make the magnifier's pan speed automatically match the user's real Windows cursor,
including mouse acceleration ("Enhance pointer precision"), instead of a fixed
`cursorSensitivity` multiplier. Moving the mouse should pan the lens exactly as far
as it would move the real cursor on the desktop.

## Background / why

Wind pans the lens by integrating **raw mouse input** (mickeys) times a fixed
`cursorSensitivity` (default 1.0). Raw mickeys are linear; the real Windows cursor
is not. Diagnosing the reporter's machine: pointer-speed slider is neutral
(`MouseSensitivity=10`, ~1.0 gain) but **acceleration is ON** (`MouseSpeed=1`, the
Windows default; `SmoothMouseXCurve` present). So slow hand motion moves the real
cursor *less* than 1:1 and fast motion *more*, while Wind does a flat 1:1 - slow
precise pans feel too fast, fast pans too slow. Because the slider is already
neutral, a linear "match the speed slider" fix would compute ~1.0 and change nothing
for this user; the entire mismatch is the acceleration curve.

Replicating Windows pointer ballistics on the raw stream (the `SmoothMouseXCurve`
algorithm) is complex, poll-rate/timing dependent, version-sensitive, and hard to
verify. Instead we let Windows do the ballistics and read the result.

### The constraint

While zoomed, Wind hides the OS cursor (`MagShowSystemCursor(FALSE)`) and
`SetCursorPos`-es it every frame to sit under the drawn cursor for click
hit-testing. So `GetCursorPos` normally reflects *our own* `SetCursorPos`, not the
user's hand - which is why the engine has used raw input. The key realization: if we
read `GetCursorPos` at the **start** of each tick (before re-setting it), the delta
from where *we* last put it equals exactly how far the OS moved the cursor from real
input in between - with full acceleration already applied. The hidden cursor's
logical position still tracks input + acceleration; hiding only affects visibility.

## Decisions (locked during brainstorming)

1. **Oracle approach.** Free desktop panning = the OS cursor's own per-tick pixel
   delta (exact acceleration match, no ballistics math). Chosen over reimplementing
   ballistics (too fragile) and over a linear-only fix (wouldn't help the accel-on
   case).
2. **Raw input retained** for two jobs: detecting a game cursor-lock, and panning
   while locked (relative-mouse mode, where linear raw is the correct feel and
   acceleration does not apply).
3. **`cursorSensitivity` repurposed** to scale raw panning *only while locked*
   (default 1.0). Free desktop panning ignores it (it must be exactly 1:1 with the OS).

## Architecture

### Per-tick flow (`main.cpp`, Win32 side; only while zoomed)

1. `GetCursorPos` -> `P`. `cursorDelta = P - lastSetVirtual`, where `lastSetVirtual`
   is the virtual-desktop point we `SetCursorPos`'d last tick. This delta carries
   Windows' acceleration.
2. Drain raw mickeys (`rawDx/rawDy`, as today). `GetClipCursor` -> is the clip rect
   confined (strictly smaller than the virtual desktop)?
3. Feed `(rawMag, cursorMag, clipConfined)` to `LockDetector` -> `locked` (bool).
4. Resolve the pan delta: **free** -> `cursorDelta`; **locked** ->
   `round(raw * cfg.cursorSensitivity)`.
5. Clamp the resolved delta to a sane per-tick maximum (so a hard flick that throws
   the OS cursor across the desktop cannot teleport the lens).
6. `mapper.update(dx, dy, level)` integrates the delta, clamps the center to the
   monitor, returns `clickDesktop` (local px).
7. `renderFrame` does `SetCursorPos(clickDesktop + origin)`; `main` stores
   `lastSetVirtual = clickDesktop + origin`.

`cursorDelta` is a delta, so it is origin-independent (same in virtual and local
space) and is fed directly to the local-space mapper.

### `LockDetector` (new pure unit: `src/lock_detector.{h,cpp}`)

Decides free vs locked from per-tick signals, with hysteresis so it cannot flap
per-tick. Pure (no `<windows.h>`), unit-tested.

```cpp
class LockDetector {
public:
    // clipConfined: a smaller-than-virtual-desktop clip rect is active (ClipCursor lock).
    // rawMag: |rawDx|+|rawDy| this tick. cursorMag: |cursorDx|+|cursorDy| this tick.
    // Returns true if the cursor is considered locked (pan from raw, not the OS cursor).
    bool update(bool clipConfined, int rawMag, int cursorMag);
    bool locked() const;
    void reset();   // back to free (call on zoom-in / recenter / retarget)
private:
    bool locked_ = false;
    int  lockStreak_ = 0;    // consecutive ticks of (raw active, cursor frozen)
    int  freeStreak_ = 0;    // consecutive ticks of (cursor tracking raw)
};
```

Logic:
- `clipConfined` true -> immediately locked (direct, reliable signal; the common
  fullscreen-game case).
- Else heuristic: a tick is "lock-evidence" when `rawMag >= kRawActive` AND
  `cursorMag <= kCursorFrozen` (mouse moving but OS cursor not). Accumulate
  `lockStreak_`; at `>= kLockTicks` consecutive, set locked. A tick is
  "free-evidence" when `cursorMag > kCursorFrozen` (cursor is moving with input);
  accumulate `freeStreak_`; at `>= kFreeTicks`, clear locked. The opposite streak
  resets when its evidence breaks. Constants chosen so normal slow accel'd desktop
  motion (cursor moves sub-pixel but accumulates within a few ticks) never reaches
  `kLockTicks`, while a truly clipped cursor (frozen indefinitely) does.

Because the caller integrates a *delta* in both regimes, a momentary wrong call only
mis-sizes one tick's delta; it never snaps the lens position. This is the structural
reason it avoids the old `Tracker` free/locked flicker (issue #3), which snapped the
absolute lens center between two derivations.

### `CursorMapper` change (`src/cursor_mapper.{h,cpp}`)

`update` integrates an already-resolved pixel delta instead of `raw * sensitivity`:

- Signature stays `MapResult update(int dx, int dy, double level)` but `dx/dy` now
  mean "pixel delta to apply to the lens center this tick" (the caller resolved
  free-vs-locked and any scaling).
- Body: `tx_ += dx; ty_ += dy;` (drop the `* sens_`), then the existing clamp + ease
  + output computation, unchanged.
- Remove the `sensitivity` constructor parameter and the `sens_` member (scaling now
  lives in the caller's locked-mode branch only). Constructor becomes
  `CursorMapper(int screenW, int screenH, double smoothing = 0.0)`.
- Sub-pixel preserved: `clickDesktop = round(cx_)` is used only for the integer
  `SetCursorPos`; `cursorDelta` subtracts the same rounded `lastSetVirtual`, so the
  fractional part of `cx_` never drifts.

### Config (`src/config.{h,cpp}`)

`cursorSensitivity` (double, default 1.0) is **repurposed**: it now scales raw-input
panning **only while a game has locked the cursor**. Free desktop panning
auto-matches the OS pointer and ignores it. Update the struct comment and the
default-ini comment to say so. No key added or removed (avoids churn; the value is
still meaningful for the locked case).

## Error handling / edge cases

- **Zoom-in / recenter / monitor-retarget:** set `lastSetVirtual` to the current
  `GetCursorPos` (first `cursorDelta` = 0) and `LockDetector::reset()` (start free).
- **Monitor edge:** `GetCursorPos` clamps at the desktop edge -> `cursorDelta`
  truncates; `cx_` clamps to `[0, monW] x [0, monH]` -> consistent, no runaway.
- **Hard flick across monitors:** the per-tick delta clamp (step 5) bounds a single
  tick's pan; `cx_` clamping bounds the destination. No teleport.
- **`SetCursorPos` vs acceleration:** Windows acceleration is velocity-based on the
  input stream, independent of absolute position, so our per-frame `SetCursorPos`
  should not perturb the accel applied to subsequent real input. This is the one
  assumption to confirm live (see Testing).

## Testing / verification

- **Pure unit tests (new + changed):**
  - `LockDetector`: clipConfined -> immediate lock; raw-active+cursor-frozen for
    `kLockTicks` -> lock; cursor-tracking for `kFreeTicks` -> free; hysteresis (a
    single contrary tick does not flip); `reset()` returns to free; slow-motion
    pattern (small raw, occasional cursor move) stays free.
  - `CursorMapper`: `update(dx,dy,level)` integrates the delta directly (no
    sensitivity scaling); clamps to bounds; smoothing eases; sub-pixel center
    retained. Update the existing sensitivity-based cases to the new delta meaning.
- **Build:** `build.bat` clean `/W4`; `build.bat test` passes (existing + new cases).
  Add `lock_detector.cpp` + `cursor_mapper.cpp` to the test build list in
  `build.bat` (lock_detector is new; cursor_mapper already there).
- **Manual (the Win32 oracle, user-run, single monitor):**
  - Slow precise pan and fast pan should now feel like the user's real cursor (the
    acceleration match) - the primary success criterion.
  - A cursor-locking game (ClipCursor) should still pan via raw and stay responsive.
  - No drift, no runaway at edges, clean zoom-in/out and recenter.
  - Confirms the `SetCursorPos`-vs-acceleration assumption holds in practice.

## Out of scope (YAGNI)

- Reimplementing Windows pointer ballistics / reading `SmoothMouseXCurve`.
- A linear pointer-speed-slider scalar (the oracle subsumes it - it matches whatever
  Windows does, slider and accel together).
- Per-game lock profiles or configurable lock thresholds (constants are internal;
  revisit only if a real game misbehaves).
- Changing the click-hit-test `SetCursorPos` behavior or the render/capture path.

## Files touched

- **New:** `src/lock_detector.h`, `src/lock_detector.cpp`, `tests/test_lock_detector.cpp`.
- **Modified:** `src/cursor_mapper.h`/`.cpp` (delta integration, drop `sensitivity`),
  `tests/test_cursor_mapper.cpp` (update the sensitivity-based cases to the delta
  meaning), `src/main.cpp` (oracle wiring: GetCursorPos delta, GetClipCursor,
  LockDetector, lastSetVirtual, per-tick delta clamp, mapper construction without
  sensitivity), `src/config.h`/`.cpp` (repurpose `cursorSensitivity` comments),
  `build.bat` (add `lock_detector.cpp` to the test build), `CLAUDE.md` (note the
  oracle + lock-fallback model).
- **Unchanged:** `render_engine.*`, `png_dump`, `hdr_info`, `cursor_decode`,
  `transform`, `zoom_controller`, `input_router`.
