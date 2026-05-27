# Configurable Zoom Experience - Design

**Branch:** `feat/zoom-config`
**Status:** approved (design), ready for implementation plan

## Goal

Add configurability to the zoom experience without changing the existing default feel:

1. **Smooth zoom** (opt-in mode): zoom-*in* starts at a slow rate and accelerates to a faster
   steady rate the longer the button is held. Two configurable scales: how much faster it gets
   (acceleration amount) and how quickly it gets there (acceleration ramp).
2. **Configurable zoom-in / zoom-out speeds** (apply to *both* linear and smooth modes): a rate
   multiplier for each direction.

The current linear, fixed-speed zoom is the default and must be reproduced exactly when all new
knobs are at their defaults.

## Background (current behavior)

`ZoomController::tick(dt)` (pure, no `<windows.h>`, unit-tested) does:

```cpp
double f = std::pow(maxLevel_ / minLevel_, dt / fullRangeSeconds_);
if (dir_ == ZoomDir::In)  level_ *= f;     // hold-in
else                      level_ /= f;     // hold-out
level_ = clamp(level_, minLevel_, maxLevel_);
```

This is a constant *geometric* rate: holding for `fullRangeSeconds` (default 1.2 s) traverses the
full range `minLevel`(1.0) -> `maxLevel`(8.0). `RunTick` calls `setDirection()` then `tick(dt)` each
frame, with `dt` already clamped to 50 ms (so a frame hitch can't jump the zoom).

## The model

Two rate multipliers scale the exponent, per direction, in **both** modes:

- Zoom in:  `level *= pow(R, dt * speedIn  / fullRangeSeconds)`
- Zoom out: `level /= pow(R, dt * speedOut / fullRangeSeconds)`   where `R = maxLevel/minLevel`

`speedIn = zoomInSpeed`, `speedOut = zoomOutSpeed`. At `1.0` this is exactly today's behavior; `2.0`
is twice as fast (full range in 0.6 s), `0.5` half.

**Smooth mode** is a *soft start* for zoom-*in* only: the in-rate eases up from a slow start to the
linear rate and **never exceeds linear**. (It does not go faster than linear; it only delays reaching
it.)

```
heldIn = continuous seconds the zoom-in button has been held (resets to 0 the moment
         the direction is not In - i.e. on release, or switching to out)

startFrac = 1 / smoothZoomAccel                              (the slow-start fraction of linear)

accelMult = 1.0                                              if linear mode
          = startFrac + (1 - startFrac) * min(heldIn / smoothZoomRamp, 1)   if smooth mode

effective speedIn = zoomInSpeed * accelMult
```

So `accelMult` ramps **linearly from `1/smoothZoomAccel` up to `1`** over `smoothZoomRamp` seconds of
continuous holding, then stays at `1`. The zoom-in rate therefore climbs from
`zoomInSpeed / smoothZoomAccel` (the slow start) up to `zoomInSpeed` (the linear rate - the cap).

- The zoom-in rate is always `<= zoomInSpeed` (the linear speed); smooth never overshoots linear.
- `smoothZoomAccel` is the **ease-in depth**: it sets how slow the start is (start =
  linear / accel). Bigger = slower start / deeper ease-in. `1` = no ease-in (= linear).
- Zoom-*out* never accelerates (constant rate `zoomOutSpeed`), in either mode.
- Releasing or reversing resets `heldIn`, so every fresh zoom-in starts slow again.
- Linear mode is exactly smooth mode with `accelMult == 1`.

## Config knobs

All added to `Config` (`config.h`), parsed in `config.cpp`, written into the default-ini text and
`tools/uiaccess_setup.ps1`, hot-reloadable. Defaults reproduce today's behavior exactly.

| Key | Default | Range (a future slider maps onto this) | Meaning |
|---|---|---|---|
| `smoothZoom` | `0` | 0 / 1 | 0 = linear (current); 1 = zoom-in soft-starts up to linear |
| `zoomInSpeed` | `1.0` | 0.25 - 4.0 | zoom-in rate multiplier (both modes; the linear/target rate) |
| `zoomOutSpeed` | `1.0` | 0.25 - 4.0 | zoom-out rate multiplier (both modes) |
| `smoothZoomAccel` | `3.0` | 1.0 - 8.0 | smooth ease-in depth: start = zoomInSpeed / this, climbs to zoomInSpeed (1 = no ease-in) |
| `smoothZoomRamp` | `0.6` | 0.1 - 3.0 (seconds) | seconds of holding to reach the linear rate |

`maxLevel` and `fullRangeSeconds` are unchanged (they still set the zoom range and base timing).

Ranges are advisory for a future GUI slider; the parser stores the raw value. `ZoomController`
defensively clamps `smoothZoomRamp` > 0 and `smoothZoomAccel` >= 1 in its math (a 0 ramp or accel < 1
must not break the curve), but does not otherwise reject out-of-range values.

## Architecture

Keep all zoom logic inside the pure `ZoomController` (unit-tested, no Win32):

- New members: `double zoomInSpeed_ = 1.0, zoomOutSpeed_ = 1.0; bool smooth_ = false;
  double smoothAccel_ = 3.0, smoothRamp_ = 0.6; double heldIn_ = 0.0;`
- New method `void setProfile(double inSpeed, double outSpeed, bool smooth, double accel,
  double rampSeconds);` - updates the speed/accel parameters in place (no level reset), so the knobs
  hot-reload while zoomed.
- `tick(dt)`:
  1. `if (dir_ == In && dt > 0) heldIn_ += dt; else if (dir_ != In) heldIn_ = 0;`
  2. existing early-out: `if (dir_ == None || dt <= 0) return;`
  3. compute `accelMult` (in-direction, smooth only) and the per-direction `speed`, then apply the
     `pow` step and clamp as today.
- `reset()` also sets `heldIn_ = 0`.

`RunTick` (main.cpp) calls `t.zoom.setProfile(cfg.zoomInSpeed, cfg.zoomOutSpeed, cfg.smoothZoom != 0,
cfg.smoothZoomAccel, cfg.smoothZoomRamp)` before `setDirection()`/`tick(dt)` each frame, so the live
config always applies (free hot-reload, no rebuild, no level reset). The constructor keeps
`(minLevel, maxLevel, fullRangeSeconds)`.

No `render_engine` changes - zoom config never touches the render params.

## Testing (pure `ZoomController` unit tests)

- **Regression:** all defaults (`smoothZoom=0`, speeds `1.0`) reproduce today's `pow` step exactly
  (in and out) - the existing zoom tests must still pass.
- **Speed multipliers:** `zoomInSpeed=2` advances the level in one tick by the square of the `1.0`
  case's factor (exponent doubled); same for `zoomOutSpeed` on the out direction; in/out independent.
- **Smooth ramp (capped at linear):** with `smoothZoom=1`, at `heldIn=0` the in-rate is
  `zoomInSpeed / smoothZoomAccel` (slow start, < linear); at `heldIn >= smoothZoomRamp` it equals
  `zoomInSpeed` (accelMult = 1, the linear rate). The in-rate never exceeds linear, and a smooth hold
  always trails an identical-settings linear hold (it eased in below it).
- **Reset:** holding in (heldIn grows), then a tick with dir != In, resets heldIn to 0, so the next
  in-tick starts slow again. `reset()` zeroes heldIn.
- **Out ignores accel:** smooth mode does not accelerate zoom-out.
- **Guards:** `smoothZoomRamp=0` and `smoothZoomAccel<1` don't divide-by-zero or invert the curve.

## Out of scope

- The GUI slider interface (future; this lands the config + behavior it will drive).
- Changing `maxLevel` / `fullRangeSeconds` semantics.
- Any non-linear *curve* shape for the accel ramp (it's a straight linear ramp of the multiplier).
