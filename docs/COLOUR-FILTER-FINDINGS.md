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
- **The pointer at 1x is not tinted or dimmed**: Windows draws it on a hardware cursor plane after
  composition, which the DWM colour effect never touches (Night light, in the display pipeline, does
  reach it). While zoomed Wind draws the cursor itself, so it is filtered there.
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
