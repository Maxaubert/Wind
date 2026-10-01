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

## Measuring the zoom response (#310, 2026-10-01)

`zoomTrace=1` logs one `zoomtrace in:` line per zoom-in (engine, warm/cold context, press->tick,
setActive split into cursor bridge and ensureMag, first present, enter-tick work, press->first DWM
composite, the next 6 ticks' work) and one `zoomtrace out:` line per zoom-out. `tools/zoom_response_ab.ps1`
measures what the user sees: press -> first visible screen change (BitBlt, transform only) and
ramp stall frames, with PresentMon and the trace lines, ABAB over txIdleReleaseMs 1200/15000.
Earlier review numbers to beat: enter tick 14-16 ms median (tail 50 ms); caret jump on ~3% of
zoom-ins (fixed by the lastSeq re-baseline).

### First results (2026-10-01, 0.15.4 branch build, transform engine, 8 zoom-ins per config)

| | desktop, still pointer | desktop, moving pointer | DOOM: The Dark Ages |
|---|---|---|---|
| press -> Wind's tick | 0.2 ms | 0.1 ms | 0.1 ms |
| enter tick (median) | 31-35 ms | 21 ms | 21 ms |
| ...of which the cursor bridge (two DwmFlush) | 29-33 ms | 19 ms | 19 ms |
| press -> first DWM composite | 38-41 ms | 27 ms | 27 ms |
| press -> visible screen change (BitBlt) | 62 ms | 42 ms | n/a (live game) |
| game frame spikes > 2x median | - | - | 0 (14.3 ms median) |

Reading: the zoom-in latency the user sees is dominated by the #221 cursor bridge in
`TransformModel::setActive(true)`: two blocking DwmFlush calls before the system cursor is blanked.
With a still pointer DWM composes lazily, so the pair waits longer (~30 ms). Ticks after the enter
tick cost ~1 ms. Earlier builds (from the #71 A/B, press -> level starts rising): 0.15.2 9.6 / 7.8 /
7.4 ms (desktop / browser / DOOM), 0.15.3 4.0 / 4.4 / 4.7 ms; the bridge and enter-tick code is the
same in all three builds. OPEN: the txIdleReleaseMs 1200 config never released the context (warm=1
on every zoom), so cold vs warm is still unmeasured; the caret-jump fix needs a typing test. Next
candidate: a non-blocking bridge (plan item B), A/B'd on blink and time-to-visible. A full suite
over all builds is planned by the owner.

### Full suite (2026-10-01): 0.15.2 / 0.15.3 / 0.15.4, each with only the zoom timeline added

Transform engine, 3840x2160 at 144 Hz, side-button zooms, 40 per build per scenario (desktop still
pointer, desktop moving pointer, DOOM: The Dark Ages in focus), ABAB over a released (cold) vs kept
(warm) magnification context. Medians; desktop rows are the median of both pointer cases.

| | 0.15.2 | 0.15.3 | 0.15.4 |
|---|---|---|---|
| press -> Wind's tick | 2.7-4.4 ms | 0.1 ms | 0.1 ms |
| press -> zoom starts (level leaves 1.0) | 8.7-10.2 ms | 5.4-6.2 ms | 5.6-6.5 ms |
| enter tick | 19.5-28.0 ms | 21.4-21.6 ms | 21.1-21.5 ms |
| ...of which the cursor bridge (two DwmFlush) | 16.6-20.3 ms | 18.9-19.9 ms | 18.9-19.8 ms |
| press -> first DWM composite | 27-34 ms | 27 ms | 27 ms |
| press -> visible screen change (desktop) | 44-50 ms | 42 ms | 42 ms |
| ramp: repeated frames in the first 300 ms | ~8 of ~43, max 1-2 in a row | same | same |
| DOOM frame time median / p99 / max | 13.4 / 14.8 / 21 ms | 13.5 / 14.8 / 20 ms | 13.5 / 14.8 / 19 ms |
| DOOM frames > 2x median | 0 | 0 | 0 |

Cold vs warm context: no difference in 0.15.3/0.15.4 (the warm flag is now read before the first
present; the earlier "always warm" reading was a measurement bug). So keeping the context alive buys
nothing on this rig and txIdleReleaseMs stays 1200 (it protects games from the #148 cursor tax).
0.15.2's slower start is the old refresh-rate idle loop (#71). The remaining big piece is the cursor
bridge: ~19 of the ~27 ms to the first composite. The earlier 8-zoom run's 62 ms still-pointer
figure did not reproduce at 40 zooms (42 ms). Caret jumps were 0, but the suite types nothing, so
the caret fix is still untested. Data: %TEMP%\wind_zoom_suite (JSON + PresentMon CSVs).

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

## The pan-start hitch: what it actually was (2026-08-27)

Symptom: the first movement after ANY pause hitches, worst at a side-to-side reversal where the
hand passes through zero velocity; then panning is smooth again. Intermittent enough per session
that single takes were worthless for most of the investigation.

CAUSE: **DWM's magnification RE-RENDER path goes cold, not the compositor.** From the per-tick
trace (`txTrace=1`) of a real session:

    prev tick: dt= 7.50ms  warm=1              <- resting, panel at full rate
    this tick: dt=25.01ms  wrote=1 changed=1   <- the FIRST REAL pan write, d(txX)=2

DWM was compositing at 143Hz right through the rest and STILL paid ~25ms the moment the magnified
source region actually moved. Only a real change to the sampled region keeps that path warm.

FIX: `txWarmMode=1` - alternate the translation by 1px on rest ticks. Field-verified on both the
desktop and in games.

COST, and the 2026-09-03 refinement (issue #246): every warm write is a real source change, so
DWM re-renders the whole magnified screen for it. Measured with `tools/gpu_ab.ps1` on the
controlled solid target (dwm.exe 3D-engine %): a zoomed session sitting still cost 16.1% with
per-tick warming, 0.0% with it off, 0.2% for native Magnifier at rest - which was the entire GPU
gap the field reported, since panning costs both magnifiers the same order (Wind 16%, native 12%
in either tracking mode). The warm write is now a PULSE on `txWarmHz` (one displacement plus its
return per period; an open pulse always closes before any other gate). Rest cost per cadence:
48Hz 10.1%, 24Hz 8.3%, 12Hz 4.6% (shipped), 6Hz 2.4%. `tools/warm_cadence_sweep.ps1` scores
each cadence with the pan-wake probe, the txtrace wake-write dt and rest GPU; on the desktop the
wake-write dt stays 7-13ms at every cadence INCLUDING warming off, so the desktop cannot set the
floor - the field verdict in a game does (12Hz: no hitch and no visible twitch reported).

### Why this took so long, and what was measured wrong

- **Composition rate is the wrong metric.** Mode 4 (perturb the LEVEL by 2e-5) held composition at
  a flat 6.94ms through every rest and scored 0.00 stalls/s in 15/15 automated rounds - and the
  user still felt the spike. A sub-pixel level nudge is not a real source change, so DWM skips the
  work and the first genuine pan write still pays. Anything that measures only WHEN DWM composited,
  and not whether it re-rendered the magnified region, will pass a build that is still broken.
- **The write cadence (`txWriteHz`/`txMinOffsetPx`) is not the answer and is actively harmful.**
  It scored well in the automated gauntlet and was field-rejected the same day: `txMinOffsetPx=2`
  advances the view in 2px steps under a smooth hand (wobble at low zoom) and `txWriteHz=60` caps
  the view at 60Hz on a 144Hz panel (reads as low fps at high zoom).
- **Do not gate the cursor sprite on "the view moved this tick".** Tried as a wobble fix; it froze
  the drawn cursor in the screen edge zones, where the source rect clamps and the transform stops
  changing while the pointer must keep travelling. Worst at bottom-left, where both axes clamp: the
  visible cursor froze while the real one kept working underneath.
- **Test in real gameplay, not a menu.** Most of this investigation ran against a DOOM menu/loading
  screen. In gameplay the game holds the mouse (raw-input mouselook) and Wind pans on the LOCKED
  path, which is a different code path entirely.

### Related but separate: VRR

The panel is variable-refresh 23-143Hz, and the transform model paces on DwmFlush by design, so
Wind's tick interval follows whatever the display is doing (measured: DOOM gameplay presents
13.68ms / 73fps, display change 13.29ms). The lens easing used to keep a fixed fraction of the gap
PER TICK, so an uneven interval changed the felt inertia every tick - a steady hand produced an
unsteady lens. Now re-derived from the MEASURED interval (`CursorMapper::setTickDeltaMs`), so the
inertia is constant in real time whatever the refresh does. Issue #223 had already fixed this for
different FIXED rates; VRR is the case it did not cover.

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

## The "native feels 144 Hz over a 70 fps game" question: measured 2026-09-28

Setup: DOOM The Dark Ages main menu (~71 fps, 14 ms frames), 4K 144 Hz VRR panel, MPO OFF
(`OverlayTestMode=5`), Wind 0.10.1 transform model with `txPace=2`. All takes driven by
`tools/pan_wake_probe.ps1 -Drive` (same injected sweep/stop pattern), DOOM foreground held.

| take | DWM composition while panning | DOOM frametime |
|---|---|---|
| native Magnifier 300% | 6.94 ms median (144 Hz) | 14.3 ms |
| Wind (`-ZoomTo 3`) | 6.94 ms (144 Hz) | 14.2 ms |
| Wind + `-DamagePin` | 6.94 ms (144 Hz) | 14.2 ms |

1. **DWM does NOT follow the game here.** With MPO off it composes at 144 Hz under both
   magnifiers, so "the composition rate drops to the game's rate" (the VRR theory in this file and
   in the 2026-09-28 code review) is not the explanation on this setup. The damage pin (a
   composited always-on-top window, the "force composed flip" idea) changes nothing, as expected.
2. **Transform write rate (`-SampleTransform`, public integer offsets only):** Wind changed the
   applied transform on 78% of compositions while panning (113/s), native on 42% (60/s). Native
   follows input 1:1 and the driven input only reached ~128 packets/s, so this does not reproduce a
   real 1000 Hz hand; it only shows Wind is not starved of writes.
3. **Screen capture at 144 fps** (ddagrab, 640x360 centre, `draw_mouse=0`): the capture itself
   tops out near 90 fps, so both arms read ~41-44 new pictures/s. The one difference: Wind held a
   picture for 3+ capture frames 11% of the time vs native 3% (more micro-holds).
   Trap: ddagrab's default `draw_mouse=1` paints the OS pointer into every frame, which inflates
   "distinct frames" for native (hardware cursor) and not for Wind (sprite, capture-excluded).

Still open, and the next thing to measure: the synthetic setup does not reproduce the felt gap.
Candidates left: (a) the CURSOR (native moves the hardware cursor at input rate; Wind blanks it
and moves a composited sprite once per tick), (b) Wind's micro-holds above, (c) input rate (a real
1000 Hz hand vs the ~128 Hz injector). Needs a real-hand take with a faster capture path (or a
phone slow-motion video), with `spriteCapturable=1` so Wind's cursor is visible to the capture.

## Open items

- Zoom-ramp spikes (~1 per cycle, 45 ms) - DWM re-scale cost during the ramp.
- CURSOR SIZE: the transform model's pointer is magnified by DWM, which violates the standing
  product rule (constant on-screen size at every zoom level). Unsolved for this model; the
  render model already satisfies it.
