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

## The pan-start hitch: FIXED (2026-08-26), and the two residuals

Symptom: zoomed panning is smooth, but the first movement after any pause hitches, worst at a
side-to-side reversal. Intermittent per session - roughly 1 session in 4 was clean with no config
change, which is what made every single-take A/B below worthless until it was understood.

### What it actually was, traced from INSIDE Wind

`txTrace=1` dumps a per-tick ring buffer at session end. Every external probe that tried to
attribute a stall was starved by the load it was measuring and reported zero writes that Wind's own
loop log contradicted. The trace settles it:

| tick state | warm OFF | warm ON |
|---|---|---|
| MOTION (changed) | 7.10 ms | 6.94 ms |
| REST (unchanged) | 11.36 ms | 6.94 ms |
| first motion tick after a rest | **15.01 ms** | 7.02 ms |
| second motion tick | **15.24 ms** | 7.00 ms |

The first motion tick after a rest WROTE the transform 8/8 times, so Wind was never the one that
stopped feeding. At rest DWM composition falls to ~88Hz and needs two frames to come back; that
~30 ms at the start of every pan is the hitch. Warm-keeping holds DWM at full rate through the
rest and removes it: 29 wake transitions, worst 8.64 ms.

### Shipped

- `txWarmMode=4` (capped 10x) - warm-keeping, above.
- `txWriteHz=60` + `txMinOffsetPx=2` - we wrote ~144/s where native writes ~49/s.
- Ramps EXEMPT from the rate cap - capping level changes gave "terrible hitching in ramp".
- Sprite moves only on ticks the view moved - else it walks across unmoved content (wobble).

Result: pan stalls 0.00/s in 7 of 7 gauntlet rounds, against 8-12/s before.

### Residual 1: VRR (not fixable in Wind)

The panel is variable-refresh, 23-143 Hz (`MinRefreshRate=23, MaxRefreshRate=143`). When the game
presents ~70 fps the panel and DWM follow it down, and the transform model is DwmFlush-paced BY
DESIGN (main.cpp: the only pace that puts the sprite and the transform in the same frame), so Wind
then updates at ~74 Hz. In one failing round warm-keeping was firing at full rate and composition
still ran at 74 Hz - no write cadence can raise a panel's refresh. The lever is on the display
side: G-SYNC/VRR off or a fixed refresh for that game, or an fps cap near the panel maximum.

### Residual 2: the overlay-plane race (open)

Wind lands composited 3/6 alt-tab sessions; native Windows Magnifier 6/6. On a plane the zoom-in
ramp costs about twice as much (8.7-10.6 stalls/s vs 3.2-4.4) and the pan walls stay up.
`mpoBuster`'s ghost is verifiably shown, fullscreen, topmost from 300 ms in - but the failing
sessions report `Hardware Composed: Independent Flip`, i.e. MPO composes MULTIPLE planes, so
covering the game with a window cannot force DWM composition. Native uses no window for this at all
(its fullscreen window is never shown); DWM simply stops promoting planes while its magnification
is active. Replicating that is the open work. Measured dead ends: asserting the ghost every 100 ms
for 2 s (2/3), the public transform channel (5/6, 3/4 on retest), never releasing the magnification
context (3/4), resting at a non-identity level (5/6).

## The pan-start hitch: ROOT CAUSE = the game on a hardware overlay plane (2026-08-26)

Field report: zoomed panning is smooth, but the FIRST movement after a pause hitches, worst at a
side-to-side reversal. Absent under native Magnifier. Crucially it is INTERMITTENT PER SESSION -
alt-tab away and back and roughly one session in four is clean, the rest stutter, with no config
change in between. That randomness is the tell, and it is what finally identified the cause.

ROOT CAUSE: whether the game is riding a HARDWARE OVERLAY PLANE for that session. On a plane
(`Hardware Composed: Independent Flip`) DWM is not compositing the game at all, so nothing Wind
writes can drive the composition rate, and the magnified view lands late on every pan start. When
the game is `Composed: Flip`, the same build with the same settings is clean. Plane promotion is a
race decided at session start, which is exactly why the symptom comes and goes.

Correlation over 36 takes (PresentMon `PresentMode`, joined against each take's stall score):

| takes | n | mean % frames on a hardware plane |
|---|---|---|
| clean (wake < 2 stalls/s) | 18 | **14.4 %** |
| stuttering (wake > 8 stalls/s) | 18 | **51.1 %** |

Two takes minutes apart, identical config: `ghost1` 4.8 % on plane -> 0.00 stalls/s; `ghost0`
35.1 % on plane -> 20.76 stalls/s.

THIS INVALIDATES SINGLE-TAKE COMPARISONS. Every A/B in this file taken while MPO was enabled has
the plane state as an uncontrolled variable, and it dominates everything else measured here. A
whole day of A/B results (write-cadence modes, keep-alive variants, input-transform republishes,
damage pins) was noise from this. ALWAYS record PresentMode alongside any magnifier timing, and
discard takes whose plane share differs from the arm being compared against.

FIXES, in order of preference:
1. **MPO off machine-wide** (`HKLM\SOFTWARE\Microsoft\Windows\Dwm` `OverlayTestMode`=5 DWORD,
   REBOOT). No planes exist, so the game is always composited and the behaviour is deterministic.
   This is also what lifts the pan walls (issue #148), so it fixes two things at once.
2. **The MPO buster ghost** (`mpoBuster=1`) is meant to force the demotion when MPO is on, and it
   DOES work when it wins - but it does not reliably win: several takes sat at ~53 % plane with the
   ghost enabled. Making the demotion deterministic (verify the plane state and re-assert until it
   takes, rather than a blind 500 ms cadence) is an open Wind bug.

## The pan-start hitch: warm-keeping experiments (superseded by the above)

The experiments below were run BEFORE the plane state was identified, so their single-take numbers
carry an uncontrolled variable. Kept because the dead ends are still informative about what DWM
does and does not respond to.

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
