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

## Auto-mode lockup (fixed 2026-07-26)

Field repro: zoom in a game, alt-tab to the settings UI, zoom out and back in -> two cursors;
return to the game and zoom is dead. Cause: the process-scoped Magnification runtime was
initialized and uninitialized independently by both models, so whichever released first broke
the other. Fixed with the `MagApiAcquire`/`MagApiRelease` refcount (see CLAUDE.md). Confirmed
fixed in the field.

Diagnostic tells for this class: **two cursors** = something released the runtime while the
render model had the pointer hidden; **cursor moves but nothing magnifies** = transform writes
returning FALSE (`txwrite ... fails=N` in wind-core.log, N == writes).

## The pan-start hitch: OPEN (investigated 2026-08-26)

Field report: zoomed panning is smooth, but the FIRST movement after the hand pauses hitches, and a
side-to-side pan hitches at each end where the hand reverses through zero velocity. Absent under
native Windows Magnifier. Reproducible on demand, and confirmed by eye on the runs the harness
scored - the metric and the user agree.

Harness: `tools/pan_wake_probe.ps1` (+ `pan_wake_ab.ps1`, `pan_reach_probe.ps1`). Wake edges come
from passive Raw Input (works while a game holds the cursor frozen), composition from blocking in
DwmFlush, game frames from PresentMon joined on QPC. It drives the sweep/stop/sweep pattern itself,
zooms closed-loop to a target level, and voids a take if the zoom did not engage or the game lost
foreground. Traps it avoids, all of which produced wrong answers first: joining PresentMon on
`TimeInSeconds` instead of `QPCTime` (put 2135/2135 rows in the wrong bucket), holding the button
for a fixed duration to set zoom (same hold gave 7.04x and 21x on consecutive takes), and opening a
magnification context in the probe itself.

| config (DOOM, 7x, identical injected hand) | idle median | idle stalls/s | wake stalls/s |
|---|---|---|---|
| Wind, `txWarmMode=0` | 13.17 ms | 58.9 | 29.1 |
| native Windows Magnifier at 700% | 6.94 ms | 0.0 | 0.0 |
| Wind, `txWarmMode=1` (1px jitter) | 6.94 ms | 0.2 | 0.0 |
| Wind, `txWarmMode=4` (level epsilon) | 6.94 ms | 0.0 | 0.0 |

### Measured dead ends (do not re-try without new evidence)

- **Re-sending the same transform** (`txWarmMode=2`): wake 23.8/s. DWM ignores an identical write,
  exactly as the old "DWM parks on static values anyway" comment claimed.
- **Republishing the input transform** (`txWarmMode=3`): wake 26.5/s - WORSE than baseline, and it
  degraded sustained motion too (3.7/s vs 0.07/s).
- **An unrelated per-frame damage source** (probe `-DamagePin`): wake 28.2/s, and it did not move
  the composition rate at all. It is not about generic damage.
- **A level change of 4e-6 relative**: too small for DWM to notice. 1e-5 registers.

### Why nothing shipped

1. **Neither working mode is visually free.** Mode 1 shifts the whole image a rigid 1 screen px at
   tick rate (the #204 shimmer). Mode 4 was believed to displace 0.077px - that is in SOURCE pixels,
   so on screen it is `0.077 * level`: ~0.6px at 7x, ~1.6px at 21x, worse than mode 1 at high zoom.
   Its applied stream also shows the derived source origin flipping a whole source pixel
   (offX 2411 <-> 2412 at 7.37x).
2. **The premise was wrong.** Sampling native's applied stream shows it writes NOTHING across a
   330ms rest - a single level value for an entire run - and still holds 6.94ms composition. Native
   is not staying smooth by keeping warm. Do not rebuild the "keep writing" theory on this evidence.
3. **The metric is bimodal on one binary.** The same build scored 0.00/s and 18-29/s wake stalls on
   consecutive takes with nothing changed. Leading suspect is VRR refresh hunting: the panel runs
   23-143Hz and composition settles at either ~144Hz or the game's ~72Hz. Until that is pinned
   down, no fix here can be called verified.

Next step for whoever picks this up: instrument what governs the composition rate (DWM timing info
/ actual display refresh) across a rest, for Wind and native side by side. The answer is in why
native holds 144Hz while writing nothing.

## Open items

- Zoom-ramp spikes (~1 per cycle, 45 ms) - DWM re-scale cost during the ramp.
- CURSOR SIZE: the transform model's pointer is magnified by DWM, which violates the standing
  product rule (constant on-screen size at every zoom level). Unsolved for this model; the
  render model already satisfies it.
