# Transform-model hitching: findings and vetted builds (issue #148)

Everything below is harness-measured over Foundation (an OpenGL city builder) on the 4K/144Hz
RTX 5090 box with MPO disabled. Game frametimes come from RTSS shared memory (`rtssread.exe`);
"spike frames" means game frames over 25 ms. Every zoom test verifies the zoom actually engaged
(the model logs `txsession ... maxLevel=` per session) - a dead keybind silently faking a clean
result was the single biggest source of false positives in this work.

Harness (scratchpad): `bench.ps1` (middle-click recipes), `hitchrun.ps1` (aggressive zoom
flicks + camera roaming), `validate.ps1` (correctness gate), `cursorwatch.exe` (what an app does
to the cursor), `maglab.exe` (Magnification API lifecycle), `rtssread.exe`, `gl_churn.exe`.

## The big one: a live magnification context taxes every cursor change

While ANY magnification context exists in the process, DWM composites magnification-aware, and
then every cursor visibility or shape change any app makes costs a re-composite. Foundation
hides and re-shows the pointer on every middle-click (`cursorwatch`: 25 visibility flips in one
20 s test), so wheel-clicks spiked frames while left-clicks were free - with Wind merely
RUNNING, never zoomed. Writing level 1.0 does NOT leave the mode; only `MagUninitialize` does.

| recipe (14 middle-click drags) | before | after |
|---|---|---|
| Wind not running (control) | 0 | 0 |
| Wind idle, never zoomed | 13-24 | 0 |
| zoom in, zoom out, then spam | 17 | 0 |
| spam while zoomed | 0 | 0 |

Shipped fix: no context and no warm-up write at startup; create on the session's first write;
park at exact identity at zoom-out; release the context 1.2 s later.

## Where the stalls actually are

- The transform WRITE is free: 0.02 ms average, 0.5 ms max (`txwrite` instrumentation logs only
  anomalies). It never blocks the tick.
- Returning DWM to identity costs a one-off compositor stall. Paying it at zoom-out (while the
  user is already in motion) instead of during the idle window removed a 120-166 ms tick stall:
  worst Wind tick 166 ms -> 14.6 ms.
- `MagUninitialize` after an identity park measures 1-2 ms.
- Remaining cost: the zoom ramps themselves, ~1 spike frame per zoom cycle (~45 ms worst) from
  DWM's re-scale work. Not yet solved.

## Where the remaining spike is: zoom-in entry

`phaseprobe.ps1` spaces the phases a few seconds apart so each lands in its own sample second.
Result across cycles: the spikes sit **exactly at zoom-in** (~35-42 ms, one per zoom), with
nothing during the hold, the pan, or the idle after. That is DWM building its magnification
machinery when the level first leaves 1.0 - the unavoidable other half of releasing the context
between sessions. Entering at a sub-pixel level first ("session warm-up") was tried and measured
WORSE (4 spikes per 3 cycles instead of 2, and it added zoom-out spikes).

## Engine comparison while zoomed at 12x, panning continuously (12 s)

| engine | game avg frametime | game spikes | Wind's own loop |
|---|---|---|---|
| render | 11.1 ms | 0 | 92 fps, 864 hitches |
| transform | 11.4 ms | 0 | **144 fps, 1 hitch** |

The game is equally happy under both; the difference is entirely in the magnifier's own
smoothness, which is why the transform model stays the default for games.

## Measured-negative experiments (do not re-try without new evidence)

- **Async transform writes**: impossible - the Magnification API is thread-affine, a writer
  thread's calls ALL fail (144/144), so Wind reports a zoom level while DWM applies nothing.
  Pointless anyway (see write cost above).
- **`txGrid`** (snap levels to a geometric ladder so DWM's per-factor surface cache hits):
  much worse - 0 spikes/22 ms continuous vs 8 spike-seconds/551 ms at 3 %, 7/583 ms at 6 %.
- **`txLevelStep`** (skip sub-threshold level changes): no better than continuous.
- **Hover sync** (one absolute cursor move per pan-rest in freeze sessions): TDRs the driver
  even with MPO off. Absolute cursor placement under an active transform is an independent
  crash trigger; clicks survive only because they are rare.
- **Innocent, measured**: input hooks, GPU priority, the cursor sprite window, the render model.

## Vetted configurations (one binary, chosen in magnifier.ini)

All three run the same deployed build; switch with the `model` key (restart Wind to apply).

| config | ini | measured |
|---|---|---|
| **A - default** | `model=hybrid` | transform in games, render on the desktop. Middle-click recipes all 0 spikes; while zoomed 144 fps / 1 hitch; one ~36 ms spike per zoom-in. Cursor magnifies with zoom (violates the cursor-size rule). |
| **B - render everywhere** | `model=render` | Middle-click recipes 0 spikes; constant-size cursor (satisfies the rule); no zoom-in spike. Cost: the magnifier's own loop runs 92 fps with many hitches while panning. |
| **C - per-app opt-out** | `model=hybrid` + the app's exe in `%LOCALAPPDATA%\Wind\churny_apps.txt` | keeps transform for other fullscreen apps (F11 video) while a specific game uses render. |

Correctness gate for A (`validate.ps1`, teardown between every session): PASS - 6/6 sessions
reach 12x, 6 releases, cursor never stranded.

## Open items

- Zoom-ramp spikes (~1 per cycle, 45 ms) - DWM re-scale cost during the ramp.
- CURSOR SIZE: the transform model's pointer is magnified by DWM, which violates the standing
  product rule (constant on-screen size at every zoom level). Unsolved for this model; the
  render model already satisfies it.
