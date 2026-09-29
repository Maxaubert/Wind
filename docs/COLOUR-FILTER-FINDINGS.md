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
