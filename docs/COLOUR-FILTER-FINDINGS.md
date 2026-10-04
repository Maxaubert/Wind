# Colour filter findings (issue #288)

> **Status.** Live. Warmth and brightness ship; design in
> [architecture/04](architecture/04-render-engine.md#colour-filters).

## Mechanism

Probe on the test machine (3840x2160, 225%), Wind stopped, `MagSetFullscreenColorEffect` with an
invert matrix at level 1 (no UIAccess needed):

| Question | Result |
|---|---|
| Does the effect apply at level 1? | Yes |
| Does GDI capture (BitBlt) see it? | Yes: centre luma 19.8 before, 235.2 after |
| Does Desktop Duplication see it? | Yes: 34.2 before, 251.8 after |
| Does it survive the process being killed? | No: Windows clears it with the process |

So while a render session is visible the DWM effect is identity and the render shader applies the
matrix (otherwise the overlay is filtered twice); the transform engine and 1x use the DWM effect.
A crashed Wind never leaves the screen filtered. Screenshots and recordings show the filter.

## Why only warmth and brightness

Field report: with the first filters on, coloured text in Windows Terminal became hard to read.
Measured on the Campbell palette (14 text colours on #0C0C0C) and a light web page: WCAG contrast of
each text colour against its filtered background, and the closest pair of filtered text colours.

| Filter | Terminal: min contrast | Terminal: closest pair | Page: min contrast |
|---|---|---|---|
| none | 2.38 | 0.146 | 3.49 |
| invert | 1.20 | 0.146 | 6.35 |
| greyscale | 1.52 | 0.002 | 4.48 |
| yellow on black | 1.43 | 0.001 | 5.57 |
| hue-keeping "smart invert" | 1.56 | 0.038 | 6.35 |
| brightness-keeping yellow tint | 1.56–1.74 | 0.001–0.094 | 3.8–3.9 |

One affine matrix on every pixel (all the DWM effect and Windows' own colour filters can do) has two
limits: squashing channels onto a ramp makes colours of equal brightness identical, and inverting
swaps light and dark in every window at once. Only a per-pixel non-linear mapping could keep each
text's contrast, and Wind has that only in the render shader. Owner decision: keep warmth and
brightness, which scale channels and never merge or swap colours. (Computed with a small Python
harness, not committed.)

## HDR and the warmth maths

- **Under HDR the DWM effect scales linear scRGB**: a green gain of 0.339 took green 2.198 to 0.746
  in an FP16 duplication. In SDR it acts on encoded values. So the same number is a far weaker tint
  under HDR.
- **Night light's scale is linear in Kelvin**: 0% = 6500 K, 50% = 3850 K, 100% = 1200 K (from its
  stored setting). Warmth uses the same scale (`WarmKelvin`).
- Night light runs in the display pipeline and is invisible to capture under HDR, so Wind models it:
  CIE 1931 blackbody gains in linear light (`kKelvinGains`), encoded for SDR and the render shader,
  linear for DWM under HDR (`BuildColorMatrix(..., linearLight)`). Brightness is decoded to linear
  under HDR so the slider looks the same in both.
- HDR state is the primary monitor's, re-read once a second while a filter is on. With mixed HDR and
  SDR monitors the single DWM matrix is right on the primary only.

## The pointer at 1x

Windows draws the pointer on a hardware cursor plane after composition, outside the DWM effect
(Night light does reach it). Wind swaps the standard pointers for tinted copies while idle
(`src/cursor_tint.*`): at warmth 100 the arrow read 246,76,0 in Desktop Duplication, size and
hotspot kept. Zoom-in swaps back in 1.1 ms. Idle restores always reload the real scheme, because
in-memory copies of the pointers are not identical to the scheme's own. A force-killed Wind leaves
the tint until its next start. Night-light apps that use the gamma ramp (f.lux, Iris) have the same
hardware-cursor problem and offer a software cursor.

## Rejected approaches

- Invert, greyscale and colour-on-black filters: contrast loss above.
- GDI gamma ramp (`SetDeviceGammaRamp`, f.lux and similar): undefined under HDR per Microsoft, reset
  by sleep and display changes, deep warm shifts need an admin registry change.
- dwm_lut: injects into dwm.exe.
- NVAPI (novideo_srgb): NVIDIA only, fails under HDR, cursor issues.
- `IDXGIOutput::SetGammaControl`: exclusive fullscreen only. `D3DKMTSetGammaRamp`: no public
  user-mode contract.
- Untested lead: an MHC2 ICC profile associated at runtime would sit in Night light's pipeline, but
  would displace the user's calibration profile and has known cursor and MPO issues.

## Design rules

- A filter holds the Magnification runtime at 1x, which taxes cursor changes in other apps; it is
  off by default. Measure a cursor-toggling game with colour on before blaming anything else.
- Colour follows the visible engine (`RenderModel::visible()`), so a pending reveal or a resting
  overlay in a handover keeps the right filter.
- The render engine filters its drawn cursor, crosshair and outline too; an inverting text beam is
  drawn untinted.
