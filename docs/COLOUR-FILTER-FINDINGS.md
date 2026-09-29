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
