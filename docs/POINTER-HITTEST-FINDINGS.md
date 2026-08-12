# Pointer-framework hit-testing under the DWM fullscreen transform (2026-08-12)

The definitive record of the transform-desktop hover dead zones: symptom, the measurement
chain, the WRONG intermediate verdict, and the proven fix. Field rounds by Max on the rig
(3840x2160@225%), instrumented probes in-tree (`probeClicks`) and standalone (scratchpad rigs).

## The symptom

With `model=transform` on the desktop and the welded cursor, hover/hit-testing had hard DEAD
ZONES: regions where the cursor was "not registered to be there or anywhere" - no hover, no
titlebar grab. Zoom-level dependent. At 4x: File Explorer's lower file rows, parts of its
titlebar, the strip above the taskbar. Legacy surfaces (Electron/Win32) were immune.

## THE FIX (field-verified 4x-20x, round 4)

**Publish `MagSetInputTransform(TRUE, srcRect, monitorRect)` per transform change while the
fullscreen transform is live** - exactly what native Magnifier does continuously (measured via
`MagGetInputTransform`: enabled=1, src tracking its pan per frame). With the source-rect input
transform published, the welded cursor gets correct hover/hit-testing in pointer-framework
apps at every position and level, and legacy apps stay correct. `magInputTransform=1` is the
knob; requires UIAccess.

The MSDN "pen and touch input" scoping on MagSetInputTransform is WRONG or incomplete:
pointer-input frameworks (XAML/DirectUI - Explorer, Settings, shell) consume it for MOUSE
pointer hit-testing under a fullscreen magnification transform. Without it published they
hit-test through the visual transform with no inverse, producing the positional dead zones.

## Why we briefly concluded the opposite (the test-matrix hole)

The A/B knob was first field-tested in mode 2 (ENABLED IDENTITY) - no effect - and the
standalone rig measured the DEFAULT (nothing published) - coordinates unmapped. Both true,
both irrelevant: the only configuration that works is the SOURCE RECT (native parity), which
sat untested while the wrong verdict ("inert for mouse") was written. The matrix that matters:

| input transform published | pointer-app hover under welded transform |
|---|---|
| none (default) | DEAD zones (positional) |
| enabled identity | DEAD zones (identical) |
| enabled source-rect -> monitor | **CORRECT everywhere** (4x-20x verified) |

## The full measurement chain (all still valid)

1. **Click-annotated probe** (`probeClicks=1`, 30 samples): at every click, dead or alive,
   `weld == GetCursorPos` exactly, DWM's applied transform matched the mapper's, no cursor
   clip, and `WindowFromPoint` found the correct window. Wind's coordinate chain exonerated.
2. **Framework split**: Electron/Win32 immune; XAML/DirectUI (DirectUIHWND list, WinUI
   DesktopChildSiteBridge titlebar) carried the dead zones.
3. **Native Magnifier control**: no dead zones on the same spots; publishes the source-rect
   input transform continuously while zoomed (sampled live).
4. **Pointer-pipeline rig** (own window + own transform + injected frames,
   EnableMouseInPointer): `SetCursorPos` NEVER generates a pointer frame (the weld is
   invisible to the pointer pipeline; WM_MOUSEMOVE only); real injected frames deliver
   identical raw pixel+himetric coordinates with the transform on or off WHEN NO INPUT
   TRANSFORM IS PUBLISHED.
5. **Weld-vs-physical trace** (`probeClicks=2`, ~36Hz, 1329 samples at 4x): the physical
   cursor tracks the weld within <=4px always, <=2px inside the dead band - pointer apps DID
   receive correctly-positioned frames in dead zones; the miss was in their hit-testing.

## Design consequences

- One default engine for desktop AND games becomes viable: welded transform + per-tick
  source-rect input transform. See the one-model spec/plan (2026-08-12).
- HARD DEPENDENCY: MagSetInputTransform needs UIAccess. Non-UIAccess runs (dev builds,
  portable use) CANNOT publish it -> transform desktop sessions there keep the dead zones ->
  the render engine must remain the desktop engine wherever UIAccess is absent, and the
  session must verify the publish SUCCEEDS before trusting the transform on the desktop.
- Diagnostics kept: `probeClicks=1/2`, `magInputTransform` modes (0/1/2), scratchpad rigs
  itprobe (input-transform sampler) and ptrprobe (pointer-pipeline coordinate rig).
