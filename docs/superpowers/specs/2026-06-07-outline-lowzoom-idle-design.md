# Edge outline: low-zoom-only and idle-hide - design

Date: 2026-06-07
Status: approved
Issue: #94 (builds on the edge outline, #92)

## Problem

The edge outline (a solid frame drawn while zoomed) is always on whenever zoom is active. Two
refinements were requested:

1. **Only at low zoom.** The outline is most useful at low zoom, where the magnified view looks
   almost like the normal desktop. At high zoom it is obvious you are zoomed, so the frame is just
   visual noise. Users should be able to limit the outline to low zoom levels.
2. **Hide when idle.** Once the outline has signalled "you are zoomed", it can fade away while the
   cursor is still, then reappear the instant the user moves. This keeps it as a transient cue
   rather than a permanent border.

Both are opt-in and off by default, so the current always-on behavior is unchanged unless enabled.

## Behavior

- **Low-zoom-only**: when `outlineLowZoomOnly` is on, the outline draws only while
  `1.0 < level <= outlineLowZoomMax`. Past the cutoff it disappears; zooming back below it returns.
  Default cutoff `2.0` (200%).
- **Idle-hide**: when `outlineIdleHide` is on, the outline fades out over `0.3s` once the cursor
  has been still for `outlineIdleSeconds` (default `7.0`). Any cursor motion resets the timer and
  restores the outline instantly (no fade-in). The fade duration is fixed at 0.3s (not configurable).
- The two are independent and compose: if low-zoom gating hides the outline, idle state is moot; if
  the outline is visible, the idle fade applies on top.
- "Cursor still" means no hand motion this tick (free-pan OS-cursor delta and raw-input delta both
  zero), so it works in both free desktop panning and game-locked (relative-mouse) panning.
- The idle timer accumulates only while zoomed and resets on each zoom-in, so every zoom session
  starts with the outline fully shown.

## Config

New fields on `Config` (`src/config.h`), parsed and clamped in `src/config.cpp`, all
hot-reloadable, all default to preserving current behavior:

```cpp
int    outlineLowZoomOnly = 0;     // 1 = only show the outline at/below outlineLowZoomMax
double outlineLowZoomMax  = 2.0;   // zoom level cutoff (clamped [1.0, 50.0])
int    outlineIdleHide    = 0;     // 1 = fade the outline out after outlineIdleSeconds of no motion
double outlineIdleSeconds = 7.0;   // idle timeout before fade (clamped [0.5, 60.0])
```

Parse branches mirror the existing keys (`stoi`/`stod`). Clamp block additions:

```cpp
c.outlineLowZoomMax  = clampd(c.outlineLowZoomMax,  1.0, 50.0);
c.outlineIdleSeconds = clampd(c.outlineIdleSeconds, 0.5, 60.0);
```

Default-ini template (`LoadConfig`) gains documented lines for the four keys, after the existing
`outlineColor` line.

## Pure logic (unit-tested)

Two small pure helpers, added to the pure section of `src/config.cpp` and declared in `config.h`:

```cpp
// Whether the outline should be shown at this zoom level, given the master toggle and the
// optional low-zoom cutoff. (The level > 1.0 "are we zoomed" gate stays in the render pass.)
bool OutlineVisibleAtLevel(const Config& c, double level) {
    if (c.outline == 0) return false;
    if (c.outlineLowZoomOnly != 0 && level > c.outlineLowZoomMax) return false;
    return true;
}

// Idle-fade alpha: full (1.0) until `idleSeconds` reaches `threshold`, then ramps linearly to 0
// over `fadeDuration`. Pure and deterministic so the fade ramp is unit-testable.
// Caller passes the accumulated idle time; this maps it to an alpha.
double OutlineIdleAlpha(double idleSeconds, double threshold, double fadeDuration) {
    if (fadeDuration <= 0.0) return idleSeconds >= threshold ? 0.0 : 1.0;
    double over = (idleSeconds - threshold) / fadeDuration;
    if (over <= 0.0) return 1.0;
    if (over >= 1.0) return 0.0;
    return 1.0 - over;
}
```

`OutlineVisibleAtLevel` folds in the master `outline` toggle, so `FillRenderParams` uses it as the
single source of truth for `p.outline`.

## Render wiring

### RenderFrameParams (`src/render_engine.h`)

Add one field:

```cpp
float  outlineAlpha;   // 0..1 fade for the edge outline (1 = solid); <=0 skips the draw
```

### FillRenderParams (`src/main.cpp`)

Replace the current `p.outline = (cfg.outline != 0);` with the gated form, and default the alpha:

```cpp
p.outline = OutlineVisibleAtLevel(cfg, level);
p.outlineThicknessPx = cfg.outlineThickness;
float orr = 0.357f, og = 0.357f, ob = 0.839f;   // #5b5bd6 fallback (accent)
ParseHexColor(cfg.outlineColor, orr, og, ob);
p.outlineR = orr; p.outlineG = og; p.outlineB = ob;
p.outlineAlpha = 1.0f;   // RunTick lowers this when idle-hide is active
```

The self-test / pacing-test harnesses call `FillRenderParams`, so they get `outlineAlpha = 1.0`
and the low-zoom gating for free.

### RunTick idle timer (`src/main.cpp`)

`TickState` gains:

```cpp
double outlineIdleSec = 0.0;   // seconds the cursor has been still (drives the outline idle fade)
```

In the zoomed branch, after `FillRenderParams(p, ...)` and after `dx/dy` (and the underlying
`curDx/curDy`, `rawDx/rawDy`) are known, compute motion and the fade:

```cpp
const bool moved = (std::abs(curDx) + std::abs(curDy) + std::abs(rawDx) + std::abs(rawDy)) > 0;
if (t.cfg.outlineIdleHide && p.outline) {
    t.outlineIdleSec = moved ? 0.0 : (t.outlineIdleSec + dt);
    p.outlineAlpha = (float)OutlineIdleAlpha(t.outlineIdleSec, t.cfg.outlineIdleSeconds, 0.3);
} else {
    t.outlineIdleSec = 0.0;   // keep it ready for when idle-hide is toggled on mid-session
}
```

Reset `t.outlineIdleSec = 0.0` on the zoom-in rising edge (where `zoomIn` is handled) so each
session starts fully shown.

`dt` is already computed at the top of `RunTick`; `curDx/curDy` and `rawDx/rawDy` are already in
scope in the zoomed branch.

### Border draw pass (`src/render_engine.cpp`)

Two changes to the existing edge-outline block in `State::render`:

1. Gate also on alpha: `if (p.outline && p.level > 1.0 && haveDesktop && p.outlineAlpha > 0.0f)`.
2. Draw with the existing alpha-blend state instead of opaque, and pass the alpha in the color:
   - replace `c->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);` with
     `c->OMSetBlendState(blend.Get(), nullptr, 0xFFFFFFFF);` (the `blend` member already used by the
     cursor pass: SrcAlpha / InvSrcAlpha).
   - change the per-edge constant `bcbv[8]` alpha from `1.0f` to `p.outlineAlpha`.

At `outlineAlpha == 1.0` the alpha blend yields `color`, so a fully-shown outline stays crisp;
below 1.0 it blends into the magnified content beneath, producing the fade.

## Config UI (`ui/src/settings-schema.js`)

Add four rows to the Display section, after the existing `outlineColor` row. No new row type is
needed (reusing `toggle` and `slider`); the generic bridge round-trips the keys.

```js
{ key:'outlineLowZoomOnly', type:'toggle', label:'Only at low zoom',
  desc:'Hide the outline once you zoom past the cutoff.', def:0, dependsOn:'outline' },
{ key:'outlineLowZoomMax',  type:'slider', label:'Low-zoom cutoff',
  desc:'Show only at or below this zoom (2 = 200%).', min:1.25, max:8, step:0.25, def:2,
  dependsOn:'outlineLowZoomOnly' },
{ key:'outlineIdleHide',    type:'toggle', label:'Hide when idle',
  desc:'Fade the outline out when the mouse is still.', def:0, dependsOn:'outline' },
{ key:'outlineIdleSeconds', type:'slider', label:'Idle timeout (s)',
  desc:'Seconds of no movement before it fades.', min:1, max:30, step:1, def:7,
  dependsOn:'outlineIdleHide' },
```

`dependsOn` greys a row out when its parent toggle is off (existing Settings.svelte mechanism;
single-key, so the cutoff/timeout sliders depend on their immediate parent toggle, not on `outline`
transitively - acceptable, matching how `smoothZoom*` rows behave).

## Testing

- **Unit (doctest):**
  - `OutlineVisibleAtLevel`: master off -> false; on + lowZoomOnly off -> true at any level; on +
    lowZoomOnly on -> true at/below cutoff, false above; boundary at exactly the cutoff -> true.
  - `OutlineIdleAlpha`: 1.0 below threshold; 1.0 at threshold; 0.5 at threshold+half-fade; 0.0 at
    threshold+fade and beyond; `fadeDuration <= 0` degenerates to a hard 1.0/0.0 step.
  - `ParseConfig`: the four new keys default correctly (0 / 2.0 / 0 / 7.0), parse, and clamp
    (`outlineLowZoomMax` and `outlineIdleSeconds` past their bounds).
- **Visual (`WIND_SELFTEST`)**: the self-test renders at 4.0x. With `outlineLowZoomOnly=1` and
  `outlineLowZoomMax=2`, the dump should show NO outline (4 > 2); with cutoff `5`, the outline
  appears. Confirms the low-zoom gating end to end.
- **Manual**: the idle fade is time-based (beyond the 20-frame self-test), so verify by hand -
  enable idle-hide, zoom in, hold the mouse still, confirm the frame fades after the timeout and
  snaps back on movement.
- **Build**: `build.bat test`, `build.bat`, `build.bat config`.

## Out of scope (YAGNI)

- Configurable fade duration / a fade-in on return (return is intentionally instant).
- Idle detection from keyboard/clicks (cursor motion only).
- A separate high-zoom-only mode (only the low-zoom direction was requested).
- Per-edge or per-monitor variation.
