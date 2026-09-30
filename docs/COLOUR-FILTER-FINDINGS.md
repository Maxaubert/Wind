# Colour filter findings (issue #288)

Spike 2026-09-29, this PC (3840x2160, 225%), Wind stopped, a standalone probe calling
`MagSetFullscreenColorEffect` with an invert matrix at level 1 (no UIAccess needed).

| Question | Result |
|---|---|
| Does the effect apply at level 1? | Yes (`set=1`, the screen inverted). |
| Does GDI capture (BitBlt) see it? | Yes: centre-block mean luma 19.8 before, 235.2 after. |
| Does Desktop Duplication (the render engine's capture) see it? | **Yes**: 34.2 before, 251.8 after. |
| Does the effect survive the process being killed? | **No**: after `TerminateProcess` with the effect on, the screen read 19.8 / 34.2 again. Windows clears it with the process. |

Consequences for the design:

- **Render engine:** its capture already contains the filtered desktop and DWM would filter its
  overlay again (invert twice = no filter). So while a render session is live, the DWM effect is set
  to identity and the matrix is applied in the render pixel shader instead. The transform engine and
  1x use the DWM effect.
- **Crash safety comes free:** a killed or crashed Wind never leaves the screen filtered. The normal
  exits still write identity so the change is immediate.
- Screenshots and recordings of the screen show the filter (both capture paths see it).

## Why there are only warmth and brightness (measured 2026-09-29)

Field report: with the filters on, coloured text in Windows Terminal became hard to read or
vanished. Measured on the Campbell palette (14 text colours on #0C0C0C) and a light web page
(black text, links, red/green/grey/orange on white): WCAG contrast of each text colour against
its filtered background, and the smallest RGB distance between any two filtered text colours.

| filter | terminal: min contrast | terminal: closest pair | page: min contrast |
|---|---|---|---|
| none | 2.38 | 0.146 | 3.49 |
| invert | 1.20 | 0.146 | 6.35 |
| greyscale | 1.52 | 0.002 | 4.48 |
| yellow on black (as first built) | 1.43 | 0.001 | 5.57 |
| same, balanced luma weights | 1.83 | 0.013 | 5.88 |
| same, 50% of the original colour kept | 1.51 | 0.057 | 5.81 |
| hue-keeping "smart invert" | 1.56 | 0.038 | 6.35 |
| brightness-keeping yellow tint | 1.56-1.74 | 0.001-0.094 | 3.8-3.9 |

Two limits, both inherent to one affine matrix applied to every pixel (all the DWM colour effect,
and Windows' own colour filters, can do):

1. Squashing three channels onto a grey or two-colour ramp makes colours of equal brightness
   identical (closest pair 0.001). Greyscale has the same flaw.
2. Anything that inverts swaps light and dark for every window at once: it helps white pages and
   wrecks dark apps (bright green in the terminal keeps 35% of its contrast under Invert).

Only a per-pixel, non-linear mapping could keep each text's contrast, and Wind has that only in
the render engine's shader (zoomed, render engine), not at 1x or on the transform engine. Owner
decision: keep only warmth and brightness, which scale channels and never merge or swap colours.
The numbers come from a small Python harness over the palette (not committed; rerun by computing
the matrix per colour and WCAG contrast in linear light).

## HDR, Night light and the warmth maths (measured 2026-09-29/30, this PC: LG OLED, Windows HDR on, SDR white 188 nits)

- **Under HDR the DWM colour effect scales LINEAR scRGB.** Desktop Duplication in R16G16B16A16_FLOAT
  with a green gain of 0.339: green 2.198 -> 0.746 (exactly x0.339). A GDI capture (SDR, after
  Windows' HDR->SDR step) had suggested sRGB-encoded maths; that was the conversion, not the effect.
  In SDR the effect acts on encoded values. So the same number means a far weaker tint under HDR:
  the first warmth build's 100% (green 0.339) was really about 2400 K on this screen, "yellow".
- **Night light's scale** (from its stored setting, CloudStore `bluelightreduction.settings`, the
  value after `cf 28` is 2 x Kelvin as a varint): 0% = 6500 K, 100% = 1200 K, 50% = 3850 K, i.e.
  LINEAR in Kelvin. Wind's Warmth now uses the same scale (`WarmKelvin`).
- **Night light is invisible to software capture under HDR** (no change in the FP16 duplication, gamma
  ramp untouched): it runs in the display pipeline. Wind therefore models it: CIE 1931 blackbody
  gains in linear light (`kKelvinGains`, Planck + Wyman-Sloan-Shirley CMF fit), encoded for SDR and
  the render shader, linear for DWM under HDR (`BuildColorMatrix(..., linearLight)`). Brightness is
  decoded to linear under HDR so the slider looks the same in both. Verified on the HDR desktop:
  warm 100% green x0.074 / blue 0; warm 50% x0.67 / x0.34; brightness 50% x0.214; 0% black.
- **The pointer at 1x is out of the DWM effect's reach**: Windows draws it on a hardware cursor plane
  after composition (Night light, in the display pipeline, does reach it). Since 2026-09-30 Wind
  swaps the standard pointers for tinted copies while idle (spec 2026-09-30-cursor-tint-design.md).
  Verified on this PC: Desktop Duplication's pointer shape reads 246,76,0 at warmth 100 (246,246,246
  off), size 64x64 and hotspot kept; zoom-in swaps back in 1.1 ms (direct, no scheme reload) and the
  engine draws + filters its own cursor; a fullscreen window in front and colour off restore the real
  scheme (reload, ~6 ms, idle only); a force-killed Wind leaves the tint until its next start heals it.
  In-memory COPIES of the pointers are not identical to the scheme's own (fixed size; the default
  text beam came back as a different format), so idle restores always reload the real scheme.
- Wind's diagnostics snapshot logs `hdr=0` regardless (logging.cpp records it conservatively); the
  engine's own `GetHdrEnabled` is the truth.

## Settings crash (RTSS), 2026-09-29

WebView2 154's browser process loads and unloads `dxgi.dll` at start-up; RTSS's global hook
(RTSSHooks64 of 2025-09-27) remembers it and, on the next window creation (Chromium's network-cost
watcher's hidden COM window), reads the unloaded DLL in `ValidateRuntimes`: an access violation that
kills the whole WebView2 engine (dump: faulting address inside "unloaded module dxgi.dll"). Chromium
creates that watcher unconditionally (`network_change_notifier_win.cc`), so there is no flag to avoid
it. Settings now recovers: `ProcessFailed` -> recreate the engine (page process: reload), at most 3 a
minute (`src/config_ui/webview_recover.h`), and the page's unapplied edits are mirrored to the host and
handed back. Verified by killing the engine with a staged change: back in ~0.3 s with the edit kept.

## How other night-light apps do it (research 2026-09-30, sources cited)

- f.lux, Iris, LightBulb, Night Ember tint with the GDI gamma ramp (`SetDeviceGammaRamp`, a 1D
  256-entry table per channel; LightBulb source: https://github.com/Tyrrrz/LightBulb).
- They have THE SAME pointer problem: f.lux's FAQ says a bright white cursor "happens when your
  videocard displays uses a 'hardware cursor'" and ships "Software mouse cursor when needed"
  (https://justgetflux.com/faq.html); Iris has "use software mouse cursor"
  (https://iristech.co/troubleshooting/). Wind's tinted-pointer swap is the same class of fix.
- They are WEAKER under HDR: Microsoft documents the gamma ramp as "undefined behavior in HDR modes"
  (https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-setdevicegammaramp); f.lux
  users report it stops working with HDR/Advanced Color
  (https://forum.justgetflux.com/topic/6163/windows-10-with-hdr-and-advanced-color-working-with-f-lux).
  Ramps are also reset by sleep/display changes (apps re-apply constantly) and deep warm shifts need
  the admin `GdiIcmGammaRange` registry change.
- Windows Night light: Microsoft says it "might also use" the post-composition display pipeline
  (3x3 linear matrix + 1D LUT), which apps can only reach through an ICC profile with the private
  `MHC2` tag (https://learn.microsoft.com/en-us/windows/win32/wcs/display-calibration-mhc). That fits
  the measurements above (invisible to capture, gamma ramp identity, hardware cursor tinted).
- Rejected for Wind: dwm_lut (injects into dwm.exe; https://github.com/ledoge/dwm_lut), NVAPI via
  novideo_srgb (NVIDIA-only, fails under HDR, its README admits cursor issues;
  https://github.com/ledoge/novideo_srgb), `IDXGIOutput::SetGammaControl` (exclusive fullscreen only),
  `D3DKMTSetGammaRamp` (no public user-mode contract).
- The one untested lead: an MHC2 profile associated at runtime
  (`ColorProfileAddDisplayAssociation`) would sit in Night light's own pipeline (HDR-capable,
  likely reaching the pointer). Unknowns: pointer behaviour (https://github.com/dantmnf/MHC2 notes a
  "buggy mouse cursor and MPO composition" with MHC active), change latency, stacking with Night
  light, and it would displace a user's own calibration profile.

## Review fixes (2026-09-30)

- The render engine's drawn pointer, Inspect crosshair and zoom outline are filtered too (cursor
  shader + CPU-filtered outline colour); an inverting text beam is drawn untinted (a matrix on an
  inverting texture changes the inversion, it does not tint).
- Colour follows the VISIBLE engine (`RenderModel::visible()`), not the selected one: a pending
  render reveal keeps the DWM-filtered desktop, the effect returns before the overlay hides at
  zoom-out and the moment an outgoing render overlay rests in a hybrid switch. Worst case left: one
  double-filtered frame while the capture catches up after the effect is cleared.
- HDR state is the PRIMARY monitor's, re-read once a second while a colour setting is on (toggling
  HDR is not guaranteed to raise WM_DISPLAYCHANGE). Mixed HDR/SDR monitors: the single DWM matrix is
  right on the primary only.
- Tinted pointer: a separate "pointers are ours" flag, so an idle restore after a render session
  (which never reloads the scheme) still reloads the user's real scheme.
- Trade-off kept: colour on holds the Magnification runtime at 1x (CLAUDE.md gotcha).
