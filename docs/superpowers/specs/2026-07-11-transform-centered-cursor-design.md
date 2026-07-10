# Transform model: centered cursor (decouple the drawn cursor from the OS cursor)

Date: 2026-07-11
Status: approved (design)

## Context

The transform model currently runs a FREE cursor: the magnification is anchored at the
cursor (`ComputeFixedPointOffset`, `T(L) == L`), so the drawn cursor moves at hand speed
across the screen while the view pans slower (by `1 - 1/level`). Two UX costs follow
directly: the cursor is not centered (a centered cursor is generally friendlier), and it
slides toward the screen edge as you move, until only a few pixels of it remain visible.

This is the third battle on this ground. The documented history:

1. **Original port** (issue #129): centered source rect with the sprite welded to the OS
   cursor position L. DWM composites the cursor AND layered windows OUTSIDE the fullscreen
   magnification (measured: a layered window at desktop P draws at screen P, unscaled), so
   the sprite sat at L while its content sat at `T(L)` = screen center. Click drift, zero
   at center, growing to the edges, dead zones at the clamps.
2. **Shipped fix** (PR #130, `14eb312`): anchor at the cursor, `off = L*(1 - 1/level)`.
   Clicks perfect everywhere; cursor no longer centered. Same trade as the old mag engine
   (`2dc5130`).
3. **Failed attempt** (`044257f`, unmerged): edge-triggered pan with a MAGNIFIED sprite.
   Dead zones returned, worse with zoom. Failure signature (drag works, hover/fresh clicks
   do not; worse with zoom) indicates sprite-placement error (a zoom-scaled sprite has a
   zoom-scaled hotspot), not a click-path error.

Ground truths this design rests on (all measured on this codebase):

- Mouse clicks land at the OS cursor's desktop position in EVERY Magnification
  configuration (`a933656`, exhaustive probes; `MagSetInputTransform` is pen/touch only).
- A layered window at desktop P draws at screen P, unscaled, regardless of the fullscreen
  transform (PR #130 marker-window measurement). We therefore control the drawn cursor's
  SCREEN position exactly.
- `CursorMapper` already computes, every tick: the centered clamped source rect
  (`srcLeft/srcTop`), the lens center (`clickDesktopX/Y` = C), and
  `cursorScreenX/Y = (center - src) * level` = `T(C)`, the screen point showing the
  lens-center content (`cursor_mapper.cpp:35`). `ComputeMagTransform` was designed to take
  the mapper's rect (`transform.h:24-30`).

## The design

Do what the render model does: decouple the drawn cursor from the OS cursor.

- **Source rect**: the mapper's centered clamped `srcLeft/srcTop` (currently ignored by
  the transform model) instead of `ComputeFixedPointOffset`.
- **Drawn cursor (sprite)**: at `cursorScreen + monitor origin`. Screen center in steady
  state; slides edge-ward only when the source rect clamps at the desktop boundary (same
  behavior as the render model). By construction this is `T(C)`: the sprite sits exactly
  on the lens-center content.
- **OS cursor**: welded to `clickDesktop` (= C), unchanged plumbing. Clicks land at C,
  which is exactly the content under the sprite. No drift at any offset, including the
  clamped edges: `cursorScreen` and the click point are linked by the same rect.
- **Zoom-in continuity**: at level slightly above 1.0 the centered rect is nearly the
  whole screen and `cursorScreen ~= C`, so the sprite starts at the cursor's real spot
  and glides toward center as the zoom deepens. `(cx - o.x) * level` is continuous in
  level; there is no snap.

This also fixes the "view pans slower than the cursor" complaint: centered mode pans the
view 1:1 with hand speed, like the render model.

### Mode selection (correct by construction)

Centered mode requires that the thing on screen is a cursor WE draw (or nothing at all).
Whenever a VISIBLE cursor exists that we cannot place, fall back to the anchored offset,
where a real cursor at L is self-consistent (`T(L) == L`):

| Situation | Offset | Cursor shown |
|---|---|---|
| `cursorSprite=1`, sprite Rendered | centered | sprite at `cursorScreen` |
| `cursorSprite=1`, drawCursor false (hide hotkey / visibility=never) or shape Hidden (app hid its cursor) | centered | none (real cursor blanked/suppressed) |
| `cursorSprite=1`, shape Unsupported (app-custom cursor: not blankable, visibly drawn by DWM at its desktop spot) | anchored | the real app-custom cursor |
| `cursorSprite=0` | anchored | the real cursor |
| `transformCenterCursor=0` | anchored | as today |

`present()` therefore calls `refreshShape()` FIRST (order swap; today the transform is set
before the sprite block) so the verdict can pick the offset for the same tick. Switching
between centered and anchored mid-zoom (a standard<->custom cursor shape transition) moves
the view in one step; this is rare, correct, and accepted.

### Config

- New key `transformCenterCursor` (int, default **1**), hot-reloadable, transform-only.
  0 = today's anchored free cursor. The old mag engine had exactly this knob
  (`magCenterCursor`, `11a2665`); given the history, the escape hatch stays one ini edit
  away.
- Config UI: toggle row "Center cursor" in the Display section, transform-only
  (`showIf model=transform`), `dependsOn: cursorSprite` (centered needs the sprite),
  desc noting the cursor stays centered while the view pans.

### Inspect mode

In centered mode the crosshair draws at `cursorScreen + origin` (the look point's screen
position under the centered rect) instead of `clickDesktop + origin`. In anchored mode the
current behavior stays (`T(L) == L` makes them the same point there). The frozen-cursor
weld (`ex.clickOverride`) is unchanged.

### Out of scope

- The render model: untouched.
- `ComputeFixedPointOffset` stays (anchored fallback + `transformCenterCursor=0`).
- Multi-monitor: the transform model already forces the primary monitor; `+ mon origin`
  is kept for correctness but no new multi-monitor behavior is added.

## Risk, stated honestly

`044257f`'s note ("geometry says clicks must be accurate for ANY offset, yet they are
not") is the one cloud. If an undiscovered OS behavior breaks clicks at non-anchored
offsets, this design hits it. The evidence points the other way (the click probes above;
the failed attempt's signature matching scaled-sprite math, which this design does not
use: the sprite stays unmagnified). The toggle bounds the damage: one ini edit restores
today's behavior. Live verification is the gate before merge.

## Testing

Unit (doctest, pure):
- `transformCenterCursor` parses, defaults to 1 (extend `test_config_model.cpp`).
- Mapper invariant pin: `cursorScreen == (center - src) * level` with the clamped centered
  rect at edges (extends existing mapper coverage; documents what the sprite placement
  relies on).

Manual (deployed transform model, `transformCenterCursor=1`):
1. Zoom in mid-screen: cursor sprite sits at screen center; moving the mouse pans the
   view 1:1 under it; the sprite stays centered.
2. Move to each screen edge/corner: the sprite slides from center to the edge exactly as
   the render model's cursor does; clicks land under the sprite tip everywhere,
   including over the taskbar area (the historical dead zone).
3. Hover: tooltips/highlights appear for the item under the sprite.
4. App-custom cursor (e.g. a game or a custom-cursor app): view steps to anchored mode,
   real cursor visible and self-consistent; back to centered when the shape returns to a
   standard one.
5. Inspect mode: crosshair centered (or edge-shifted per the clamp), look-point panning
   and click routing unchanged.
6. `transformCenterCursor=0` hot-reload: behavior returns to today's free cursor.
7. Render model: unchanged.

## Issue / PR mapping

One issue, one branch (`feat/transform-centered-cursor`), one PR.
