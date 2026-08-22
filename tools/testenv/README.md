# Wind proving ground

Automated, reusable test environment (issue #225): drives Wind with injected input over
controlled backdrop windows and measures what usually breaks - pacing, centering, wobble,
zoom-ramp smoothness, RAM. Fully self-driving; a human is only needed to *watch* (the grid
backdrops make flicker and stutter easy to see by eye).

## Running

    powershell -File tools\testenv\run.ps1 -Suite rapid          # ~1 min smoke while iterating
    powershell -File tools\testenv\run.ps1 -Suite quick          # ~2 min for riskier changes
    powershell -File tools\testenv\run.ps1 -Suite full           # ~8 min pre-PR gate
    powershell -File tools\testenv\run.ps1 -Suite soak -Minutes 30
    powershell -File tools\testenv\run.ps1 -Suite full -CI       # exit 1 on regression vs baselines
    powershell -File tools\testenv\run.ps1 -Suite full -UpdateBaseline

**Iteration gate**: `rapid` or `quick` by change risk -> `full` -> PR. Run `stress` before
releases and after engine-level work - its scenarios exist to break the magnifier (overzoom
held far past maxLevel while panning, 64-mickey slam pans, violent flick bursts, rapid
zoom-storm cycling), and its primary verdict is the health check: Wind still alive, dwm.exe
not restarted, no device-lost in the log, level never escaped the configured cap.

## The protocol (per suite)

1. Wind is restarted with telemetry enabled; a **full zoom-out reset** runs first (never trusts
   prior state), and every scenario starts with the cursor at the **same position** (monitor
   centre) for reproducibility.
2. **Start tone** (880 Hz, short) - hands off the mouse from here.
3. Scenarios run: backdrop spawns and takes focus -> zoom in -> movement program -> full reset.
4. **Stop tone** (440 Hz, long) - the hands-off period is over. These are the only two sounds
   in the environment; a failed run still ends with the same stop tone.
5. Wind is restarted clean (telemetry off).

## Backdrops and programs

Backdrops (all fullscreen, grid-painted for the human eye): `solid`, `white` (no grid -
worst case), `acrylic` with a **strength ladder** (`glass` = near-pure blur, `light`, `mid`,
`heavy` = the Prism-class tint), `animated` (scrolling grid, game-like motion). Borderless ->
hybrid picks the transform engine; captioned -> render-class.

Acrylic scenarios always run over a controlled **underlay window** - the blur samples whatever
is behind the acrylic surface, so without one the desktop leaks into the measurement. `solid`
underlay = the cheap static case (DWM re-uses the blur); `animated` underlay = video-like
motion beneath, which forces a re-blur every composite - the expensive acrylic case, and the
`acryl-heavy-video` vs `acryl-heavy-zigzag` pair measures exactly that difference.
NOTE: with `desktopTransform=1` in the live ini, everything runs transform - the results table
reports the OBSERVED engine per scenario.

Programs: `zig` (top<->bottom zig-zag), `pan` (medium), `fast`, `drift` (1-mickey precision
circle - where wobble hides), `hold` (dead stop - wobble at rest), `rezoom` (5 in/out cycles).

## Telemetry

Wind writes one CSV line per tick when enabled (`src/test_telemetry.h`): level, engine, mapper
centre, cursor, welded flag, tick duration, QPC timestamp. The runner's phase marks use the
same QPC clock, so scenarios index directly into the file. Enablement travels via
`%LOCALAPPDATA%\Wind\testlog.txt` (a control file - the signed uiAccess build launches
brokered, so env vars do not survive; dev builds also honor `WIND_TESTLOG`).

Metric notes:
- `dtP95/dtP99/hitches`: tick pacing during the movement program.
- `devMed/devP95` (cursor vs lens centre) and `jitP95` (per-tick change of that gap) are ~0 BY
  CONSTRUCTION in free-cursor follow mode - they become meaningful in weld mode (game locks)
  and render-engine sessions. Zero there is not a broken metric.
- `maxLevel` confirms the zoom actually engaged (a ~1.0 value means the ramp was blocked -
  see the launch-quiesce note in lib.ps1's Start-Backdrop).
- RAM delta is across the whole suite; the rezoom scenarios are the leak amplifiers.

## Baselines and CI

`baselines.json` stores per-scenario PASS numbers from this rig (`-UpdateBaseline`). `-CI`
compares with tolerance (1.6x + slack) and exits nonzero on regression. Absolute floors
(dtP99 > 25 ms, jitP95 > 25 px, backSteps in a non-rezoom scenario) catch catastrophes even
without a baseline.

GitHub-hosted runners cannot run this (no real GPU/display for Desktop Duplication or the
Magnification API), so the push-to-main story is local: `hooks/pre-push.sample` (reminder
gate) and `register_nightly.ps1` (scheduled full run on this rig).

## Files

- `run.ps1` - orchestrator: suites (incl. stress), scenarios, health checks, verdicts, JSON
- `lib.ps1` - interop (injection, tones, Wind lifecycle, backdrop mgmt) + telemetry analysis
- `backdrop.ps1` - one backdrop window per child process
- `probe_animated.ps1` - standalone zoom-engagement probe (shakedown diagnostic)
- `baselines.json`, `results/` - this rig's truth (results/ is gitignored)
