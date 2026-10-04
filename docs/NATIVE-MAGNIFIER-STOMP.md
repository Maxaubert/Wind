# The built-in Magnifier stomps the transform engine (issue #217)

> **Status.** Closed. The stomp guard shipped (PR #218). Picking Render while a foreign magnifier
> runs is parked. Design: [architecture/05](architecture/05-transform-engine.md#the-input-transform).

## Symptom

In a transform session the cursor stopped sitting at the view centre: it trailed the view while
panning and caught up at rest ("wobble with inertia"). Measured with `tools/mag_wobble_probe.ps1`
at a constant 900 px/s pan at ~8x: the sprite sat a constant ~276 screen px behind the content
(~36 ms, about 5 ticks), collapsing to zero at rest. Wind's own `cursor divergence` log read
0–9 px throughout, because it samples at the weld instant.

## Root cause

The built-in Magnifier (Magnify.exe) was running, even unzoomed. Reproduced with
`tools/mag_wobble_repro.ps1 -WmOpen` and `tools/mag_wobble_monitor.ps1`:

- Magnify.exe at 1x continuously publishes an enabled identity input transform
  (`enabled=1 src=(0,0,3840,2160) dst=(0,0,3840,2160)`).
- Wind zoomed with Magnify.exe open: the slot stays identity. A magnified desktop with an identity
  input transform gives hover dead zones ([POINTER-HITTEST-FINDINGS.md](POINTER-HITTEST-FINDINGS.md)).
- Magnify.exe closed cleanly (Win+Esc): the slot clears at once.
- Magnify.exe killed while zoomed: its last rect stays enabled system-wide and survives Wind and
  DWM restarts, until a later publish overwrites it.
- The visible wobble itself is sprite move latency: Magnify.exe's second magnification context
  makes DWM apply the sprite's per-tick move late relative to the transform write.

## Shipped: the stomp guard

Every zoomed tick reads the input transform back (`MagGetInputTransform`, ~0.1 ms) and republishes
when it is not Wind's last publish; the same tick re-asserts `MagShowSystemCursor(FALSE)`. Success
is judged by read-back, because `MagSetInputTransform` can return FALSE while the publish lands.

## Verdicts

- Against a running Magnify.exe the guard loses every race (144 stomps a second). It heals a rect
  stranded by a killed Magnify.exe (one republish wins) and reliably detects a foreign writer.
- The wobble reproduced on the fixed build, so the identity input transform is a symptom of
  Magnify.exe running, not the wobble mechanism.
- `spriteBand16=1`: negative. Band-16 windows are magnified like everything else on build 26200;
  the screen-space sprite was misplaced.
- `cursorSprite=0` (raw welded cursor): negative for Transform. The cursor plane composites
  outside the magnification and points at the wrong content.
- `model=render` with Magnify.exe open: clean. Render draws its cursor in its own frame and is
  immune to these shared-state stomps.

## Parked

Auto picks Render while a foreign magnifier is detected (the guard's detection latch plus a
Magnify.exe check at zoom-in), through the existing mid-zoom switch; a pinned `model=transform`
gets a warning. Not built.
