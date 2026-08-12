# One default engine: transform on the desktop - design

Date: 2026-08-12
Status: approved-pending-review
Foundation: docs/POINTER-HITTEST-FINDINGS.md (the dead-zone root cause and its fix)

## Goal

Make the transform engine viable as the ONE default magnification experience - desktop and
games - by shipping the proven fix: welded cursor + per-change source-rect
`MagSetInputTransform`. Converge gradually; retire nothing until the field says so.

## What changed today

Welded transform sessions on the desktop had hard hover dead zones in pointer-framework apps
(Explorer/Settings/shell). Root cause: those frameworks consume the system input transform for
mouse pointer hit-testing under a fullscreen magnification transform (MSDN scopes the API to
pen/touch - wrong). Publishing `MagSetInputTransform(TRUE, srcRect, monitorRect)` on every
transform change (native-Magnifier parity) fixes hover everywhere, 4x-20x field-verified,
with legacy apps unaffected.

## Constraints that shape the design

1. **UIAccess dependency**: MagSetInputTransform fails without UIAccess. Dev builds and any
   non-UIAccess run CANNOT fix the dead zones -> the transform must VERIFY the publish
   succeeds per session, and the desktop pick must fall back to render when it does not.
2. **MPO**: the pan wall stays for transform game sessions on MPO-enabled machines
   (unchanged). Desktop sessions were always full-range.
3. **Cursor size**: the transform sprite is DWM-magnified (grows with zoom) - still the open
   violation of the constant-size rule. Phase 2 tests the band-16 escape; its outcome does
   NOT gate Phase 1 (the render desktop default is unchanged until the flip decision).
4. **Render stays**: non-UIAccess fallback, multiMonitor=1 (transform is desktop-wide), and
   the sharpness/bilinear knobs live there. "One model" means one DEFAULT experience, not
   deleting engines.

## Phases

**P1 - productionize the input transform (this plan).**
- The transform model publishes the source-rect input transform on EVERY transform change in
  EVERY session (game and desktop) whenever it is available; clears it at session end
  (existing) and on shutdown.
- Availability probe at session start: one publish attempt; failure -> logged once, session
  marked `inputTransformOk=false`.
- Rect math extracted pure (`ComputeInputTransformRects`: src from srcLeft/srcTop + monitor
  extent/level, dst = monitor rect WITH origin) + doctests. The current code uses a
  0,0-based dst; wrong off-primary.
- New ini knob `desktopTransform` (default 0): hybrid's engine pick treats "desktop with
  desktopTransform=1 AND input transform verified" as a transform pick. engine_pick.h gains
  the two inputs; doctests updated. `magInputTransform` becomes an internal diagnostic
  (modes 0/2 kept for A/B); the shipped default flips to source-rect-on inside transform
  sessions regardless of the knob.
- Settings UI: `desktopTransform` as an advanced toggle ("Use the game engine on the desktop
  (experimental)").

**P2 - constant-size cursor experiment (banded sprite).**
Create the cursor sprite via CreateWindowInBand band 16 positioned in SCREEN space
(cursorScreen). Hypothesis: high-band windows escape the DWM fullscreen transform (native
Magnifier's own fullscreen UI stays unmagnified). If confirmed: crisp constant-size centered
cursor for ALL transform sessions - the cursor-size rule is met and the last UX gap vs render
closes. If refuted: fallback candidates are the 1/level-scaled sprite (constant size, soft at
high zoom) or accepting scale-with-zoom on the transform path (rule stays open). Kill
criterion: banded window is transformed like everything else -> keep current sprite.

**P3 - parity + endurance (gates the default flip).**
- Cursor-shape churn tax on the desktop: measure re-composite cost of I-beam/hand churn while
  zoomed (the live-context tax class) vs render; regression -> stay opt-in.
- Outline: skip on transform in v1 (render-only), unless P2's banded window lands (it could
  draw the outline unmagnified for free).
- Zoom-ramp spike comparison desktop transform vs render (the known ~45ms ramp spikes).
- DRM check (Netflix): if transform shows protected content like native Magnifier does, the
  magnify model becomes redundant -> separate retirement decision later.
- Weeks of field use with desktopTransform=1 on the rig.

**P4 - the flip.** If P3 holds: desktopTransform default flips to 1 for UIAccess installs
(render remains the automatic fallback wherever the input transform cannot be verified).
Nothing is deleted.

## Error handling

- Publish failure mid-session (e.g. transient): log once per session, keep the session on
  transform (games unaffected by the dead zones anyway); the DESKTOP pick requires verified
  availability up front.
- Session end/shutdown always clears the input transform (a stale system-wide input mapping
  would corrupt pointer input at 1x system-wide - same invariant class as cursor restore).

## Testing

- Pure: ComputeInputTransformRects doctests (origin offsets, level edges, rounding);
  engine_pick doctests for the two new inputs.
- Field: the TransformProbe profile flow (probeClicks 1/2) IS the validation harness for the
  dead zones; endurance via normal daily use with desktopTransform=1.

## Out of scope (YAGNI)

- Retiring render or magnify (separate decisions after P3/P4 evidence).
- multiMonitor transform sessions (desktop-wide transform vs per-monitor semantics).
- The MPO-buster window (ROADMAP item, unchanged).
- Free-cursor desktop mode (proven viable, rejected UX).
