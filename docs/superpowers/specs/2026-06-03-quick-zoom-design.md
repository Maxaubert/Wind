# Quick zoom (double-tap toggle) - design

Date: 2026-06-03
Status: approved (brainstorm); pending implementation plan
Related: `src/zoom_controller.{h,cpp}`, `src/main.cpp` (`RunTick`), `src/config.{h,cpp}`,
`ui/src/settings-schema.js`

## Summary

A double-tap on either zoom key (zoom-in or zoom-out, mouse side-button or keyboard, primary or
alternate binding) toggles the magnifier between fully zoomed out (1.0x, "0%") and a remembered
zoom level. Both keys do the same thing.

- Zoomed in -> double-tap -> snap out to 1.0x. The level being left is remembered as the
  "last quick-zoom level", but ONLY if it was more than 200% (level > 2.0).
- At 1.0x -> double-tap -> snap in to the last remembered level (or a configurable default of 4x
  if nothing has been remembered yet this session).
- Levels at or below 200% are never remembered, so a quick-tap out from a shallow zoom does not
  overwrite a previously remembered deeper level.

Example (consistent with the user's intent): manually zoom to 540% -> double-tap -> 0% (540%
remembered) -> double-tap -> 540%. If at less than 200% -> double-tap -> 0% (nothing remembered)
-> double-tap -> the previously remembered level (or 400% default if none).

Internally the zoom level is a raw multiplier where 1.0 = no magnification ("0%"), 2.0 = "200%",
etc. All thresholds below are stated in that multiplier space.

## Decisions (from brainstorming)

- Hold-vs-double-tap: keep hold-to-zoom unchanged; detect the double-tap independently on top, and
  let quick-zoom win. The tiny ramp produced by the two taps is immediately overridden by the snap.
- Transition: instant snap (no animated glide).
- Unset target: use a configurable default level (default 4x / 400%).
- Persistence: in-memory per session (no core-side ini write); resets on app restart.
- Binding scope: all configured zoom bindings are watched (mouse side-buttons + keyboard, primary +
  alternate). A double-tap is two quick taps of the SAME channel; double-tapping either the zoom-in
  or zoom-out channel fires the same toggle.
- Default level: 400% (4x), configurable.
- Config knobs: an enable toggle (default on) plus an adjustable double-tap window (default 300 ms).
- The 200% remember-threshold is a fixed constant (not user-configurable).

## Architecture

The work splits cleanly into pure logic (unit-testable, no `<windows.h>`) and the Win32 tick glue,
matching the project's existing separation. The pure logic lives in `zoom_controller.{h,cpp}`,
which is already compiled into both the app build (`src\*.cpp`) and the test build (`build.bat`
line 81), so no build-file changes are needed.

### 1. Pure logic - `zoom_controller.h/.cpp`

`ZoomController::setLevel(double l)`
: Clamp `l` to `[minLevel_, maxLevel_]` and set `level_` directly. This is the instant snap.
  `dir_` is left untouched: after a double-tap the keys are released, so `ResolveDirection`
  returns `None` on the next tick and `tick()` does nothing. (If the user keeps the second tap
  held, hold-to-zoom resumes ramping from the snapped level, which is acceptable.)

`QuickZoomDetector`
: Two independent channels (in, out). Each remembers the timestamp of its last down-edge.

```
class QuickZoomDetector {
public:
    void setWindow(double seconds);                 // double-tap window
    bool update(bool inEdge, bool outEdge, double nowSeconds); // fires once per completed double-tap
    void reset();
private:
    double window_ = 0.3;
    double lastInDown_  = -1e9;
    double lastOutDown_ = -1e9;
};
```

`update` logic, per channel whose edge fired this tick:
- if `nowSeconds - lastDown <= window_`: set `fire = true` and consume (`lastDown = -1e9`).
- else: record `lastDown = nowSeconds`.
- return `fire`.

Two down-edges of the same channel within the window fire exactly once; consuming resets the
channel so a triple-tap is one double-tap then a fresh start. A double-tap of either channel
returns `true`, and the caller applies the same toggle regardless of which channel fired.

### 2. Toggle application - `RunTick` (`src/main.cpp`)

Placed AFTER `t.zoom.tick(...)` and BEFORE `double lvl = t.zoom.level();`, so the snap flows
through the same-tick zoom-in/zoom-out transition logic that already keys off `lvl` vs
`t.prevLvl`. No rendering special-casing: snap-out (to 1.0) triggers the existing zoom-out
transition (overlay hide, cursor restore); snap-in triggers the existing zoom-in transition
(monitor retarget, mapper reset to cursor, cursor hide, capture invalidate, reveal).

```
inEdge  = inHeld  && !t.prevInHeld;
outEdge = outHeld && !t.prevOutHeld;
t.prevInHeld = inHeld; t.prevOutHeld = outHeld;
if (t.cfg.quickZoom) {
    t.quickZoom.setWindow(t.cfg.quickZoomWindowMs / 1000.0);   // live, like setProfile
    double nowSec = double(now.QuadPart) / double(t.freq.QuadPart);
    if (t.quickZoom.update(inEdge, outEdge, nowSec)) {
        double cur = t.zoom.level();
        if (cur > 1.0 + kEps) {                  // zoomed -> snap out to 0%
            if (cur > kQuickZoomStoreThreshold)  // > 2.0 (200%)
                t.quickZoomStored = cur;
            t.zoom.setLevel(1.0);
        } else {                                 // at 0% -> snap in
            double target = (t.quickZoomStored > 0.0)
                              ? t.quickZoomStored
                              : t.cfg.quickZoomDefault;
            t.zoom.setLevel(std::min(target, t.cfg.maxLevel));
        }
    }
}
```

`nowSec` uses the QPC value already read at the top of `RunTick` (`now`, `t.freq`).
`kQuickZoomStoreThreshold = 2.0`. `quickZoomStored = 0.0` is the "nothing remembered yet"
sentinel (it is only ever set to a value > 2.0, so 0 unambiguously means unset).

The store/restore arithmetic (the > 200% rule, the default fallback, the maxLevel clamp) is
extracted into a tiny pure free function so it can be unit-tested without the Win32 tick:

```
// pure, in zoom_controller
struct QuickZoomResult { double newLevel; double newStored; };
QuickZoomResult ApplyQuickZoom(double cur, double stored, double def, double maxLevel);
```

New `TickState` fields: `double quickZoomStored = 0.0;`, `bool prevInHeld = false;`,
`bool prevOutHeld = false;`, `QuickZoomDetector quickZoom;`.

### 3. Config + UI

`Config` (src/config.h) gains, with defaults preserved when the key is missing:
- `int    quickZoom         = 1;`     // enable (default on)
- `int    quickZoomWindowMs = 300;`   // double-tap window in milliseconds
- `double quickZoomDefault  = 4.0;`   // level used when nothing has been remembered yet

Parsed in `config.cpp` alongside the existing keys. All three are hot-reloadable: the tick reads
`cfg.quickZoom`/`cfg.quickZoomDefault` directly and re-applies the window each tick via
`setWindow`, mirroring how the zoom profile is applied live.

UI (`ui/src/settings-schema.js`), in the existing **Zoom** section:
- toggle: "Quick zoom (double-tap)" -> `quickZoom`, def 1
- slider (gated on `quickZoom`): "Double-tap window (ms)" -> `quickZoomWindowMs`, e.g. 150-600,
  step 25, def 300
- slider (gated on `quickZoom` via `dependsOn`): "Quick-zoom default" -> `quickZoomDefault`,
  min 2, max 50 (matching the maxLevel slider range), step 0.5, def 4.0

The WindConfig bridge round-trips arbitrary config keys, so no bridge change is required.

### 4. Tests (`tests/`, pure)

`QuickZoomDetector`:
- double-tap inside the window fires exactly once
- two taps outside the window do not fire
- channels are independent (in-tap then out-tap does not fire)
- either channel double-tapped fires
- triple-tap = one fire, then the third tap starts a fresh sequence
- a changed window via `setWindow` is respected

`ZoomController::setLevel`: clamps to `[min, max]`.

`ApplyQuickZoom`:
- zoomed and cur > 2.0 -> newLevel 1.0, newStored = cur
- zoomed and cur <= 2.0 -> newLevel 1.0, newStored unchanged (shallow zoom not remembered)
- at 1.0 with a stored level -> newLevel = stored (clamped to maxLevel)
- at 1.0 with nothing stored -> newLevel = default (clamped to maxLevel)

## Out of scope / non-goals

- No animated glide (instant snap only).
- No persistence across restarts (in-memory only).
- The 200% remember-threshold is not user-configurable.
- No mixed-channel double-tap (an in-tap followed by an out-tap does not trigger).

## Edge cases

- Snapping out hides the overlay and restores the OS cursor via the existing zoom-out transition.
- Snapping in to a level above `maxLevel` is clamped to `maxLevel`.
- If the user holds the second tap of a double-tap, hold-to-zoom resumes from the snapped level
  (acceptable; quick-zoom still landed the snap first).
- Very fast taps (< one tick, ~7 ms at 144Hz / ~16 ms at 60Hz) are below human double-tap speed,
  so tick-rate edge detection is sufficient; no hook-thread timing is needed.
