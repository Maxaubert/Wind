# Wind proving ground

Automated, reusable test environment (issue #225): drives Wind with injected input over
controlled backdrop windows and measures what usually breaks – pacing, centering, wobble,
zoom-ramp smoothness, RAM. Fully self-driving; a human is only needed to *watch* (the grid
backdrops make flicker and stutter easy to see by eye).

## Running

    pwsh -File tools\testenv\run.ps1 -Suite rapid          # ~1 min smoke while iterating
    pwsh -File tools\testenv\run.ps1 -Suite quick          # ~2 min for riskier changes
    pwsh -File tools\testenv\run.ps1 -Suite full           # ~8 min pre-PR gate
    pwsh -File tools\testenv\run.ps1 -Suite soak -Minutes 30
    pwsh -File tools\testenv\run.ps1 -Suite full -CI       # exit 1 on regression vs baselines
    pwsh -File tools\testenv\run.ps1 -Suite full -UpdateBaseline

**Iteration gate**: `rapid` or `quick` by change risk -> `full` -> PR. Run `stress` before
releases and after engine-level work – its scenarios exist to break the magnifier (overzoom
held far past maxLevel while panning, 64-mickey slam pans, violent flick bursts, rapid
zoom-storm cycling), and its primary verdict is the health check: Wind still alive, dwm.exe
not restarted, no device-lost in the log, level never escaped the configured cap.

## The protocol (per suite)

1. Wind is restarted with telemetry enabled; a **full zoom-out reset** runs first (never trusts
   prior state), and every scenario starts with the cursor at the **same position** (monitor
   centre) for reproducibility.
2. **Start tone** (880 Hz, short) – hands off the mouse from here.
3. Scenarios run: backdrop spawns and takes focus -> zoom in -> movement program -> full reset.
4. **Stop tone** (440 Hz, long) – the hands-off period is over. These are the only two sounds
   in the environment; a failed run still ends with the same stop tone.
5. Wind is restarted clean (telemetry off).

## Backdrops and programs

Backdrops (all fullscreen, grid-painted for the human eye): `solid`, `white` (no grid -
worst case), `acrylic` with a **strength ladder** (`glass` = near-pure blur, `light`, `mid`,
`heavy` = the Prism-class tint), `animated` (scrolling grid, game-like motion). Borderless ->
hybrid picks the transform engine; captioned -> render-class.

Acrylic scenarios always run over a controlled **underlay window** – the blur samples whatever
is behind the acrylic surface, so without one the desktop leaks into the measurement. `solid`
underlay = the cheap static case (DWM re-uses the blur); `animated` underlay = video-like
motion beneath, which forces a re-blur every composite – the expensive acrylic case, and the
`acryl-heavy-video` vs `acryl-heavy-zigzag` pair measures exactly that difference.
NOTE: with `desktopTransform=1` in the live ini, everything runs transform – the results table
reports the OBSERVED engine per scenario.

Programs: `zig` (top<->bottom zig-zag), `pan` (medium), `fast`, `drift` (1-mickey precision
circle – where wobble hides), `hold` (dead stop – wobble at rest), `rezoom` (5 in/out cycles).

## Telemetry

Wind writes one CSV line per tick when enabled (`src/core/test_telemetry.h`): level, engine, mapper
centre, cursor, welded flag, tick duration, QPC timestamp. The runner's phase marks use the
same QPC clock, so scenarios index directly into the file. Enablement travels via
`%LOCALAPPDATA%\Wind\testlog.txt` (a control file – the signed uiAccess build launches
brokered, so env vars do not survive; dev builds also honor `WIND_TESTLOG`).

Metric notes:
- `dtP95/dtP99/hitches`: tick pacing during the movement program.
- `devMed/devP95` (cursor vs lens centre) and `jitP95` (per-tick change of that gap) are ~0 BY
  CONSTRUCTION in free-cursor follow mode – they become meaningful in weld mode (game locks)
  and render-engine sessions. Zero there is not a broken metric.
- `maxLevel` confirms the zoom actually engaged (a ~1.0 value means the ramp was blocked -
  see the launch-quiesce note in lib.ps1's Start-Backdrop).
- RAM delta is across the whole suite; the rezoom scenarios are the leak amplifiers.

## Benchmark mode (bench.ps1): compare magnifiers head to head

    pwsh -File tools\testenv\bench.ps1                       # Wind vs Windows Magnifier
    pwsh -File tools\testenv\bench.ps1 -Drivers wind,native,external -ExternalSpec zt.json

Drives DIFFERENT magnifiers through the SAME scenarios (solid pan/fast, heavy acrylic zigzag
over a solid underlay, heavy acrylic pan over a video-like animated underlay, and a response
probe) and prints a game-benchmark scoreboard per magnifier: avg view fps (offset-update
rate), 1% low (from the p99 inter-update gap), composition fps, response time med/p95 (input
impulse -> view reaction), RAM avg/max, CPU avg, wobble p95. Full JSON per run plus one line
per magnifier appended to `results/catalog.csv` - the comparable history.

Rules and limits:
- ONE magnifier at a time, enforced per driver (issue #217: two magnification contexts share
  DWM state and poison each other's numbers). Wind is stopped for native runs and vice versa;
  the native Magnifier's registry is backed up and restored; Wind is restarted at the end.
- Measurement rides the DWM fullscreen-transform readback (MagGetFullscreenTransform), which
  covers Wind's transform engine, the native Magnifier, and fullscreen-mode AT magnifiers.
  Wind must therefore be running its transform engine for bench runs (desktopTransform=1 -
  true on the dev machine). Render-engine sessions are invisible to this channel.
- The native driver zooms via ONE registry write (which native eases cleanly – the measured-
  safe channel); Wind zooms closed-loop by holding the side button until the readback reaches
  the target; external magnifiers press their configured hotkey chord until the level lands.
- External spec JSON (ZoomText etc.): name, exe, procNames, zoomInVks/zoomOutVks (the chord),
  startWaitMs. The external program is left running afterwards (it was likely there first).

## Displays: TV, virtual screens and the matrix

The suite needs a live display: with the TV off or asleep, DWM drops to about 13 Hz and every
timing is noise. `run.ps1 -Display` decides what to do about it:

| `-Display` | What happens |
|---|---|
| `auto` (default) | TV on: run on it as always. TV off or asleep: a virtual screen becomes primary for the run, if the driver is installed; otherwise the old abort (exit 4) |
| `virtual` | Always a virtual primary. Set it with `-DisplayMode WxH@Hz,scale` (default `3840x2160@120,100`) and `-VirtualCount 2` for a second screen |
| `tv` | Never switch |

    pwsh -File tools\testenv\matrix.ps1                     # rapid across 6 setups, about 8 min
    pwsh -File tools\testenv\matrix.ps1 -Only 1080p,dual
    pwsh -File tools\testenv\display.ps1 -List
    pwsh -File tools\testenv\display.ps1 -Restore           # after a crashed run

The virtual screen must be primary because the transform engine magnifies the primary monitor only.
The layout is saved to `results/display-layout.json` before any change and restored after each run.
If the TV is still off at that point, the virtual screen stays and the snapshot is kept: run `-Restore`
when the TV is back. Virtual monitors are detached from the desktop between runs.

**Idle state (#571).** The virtual screen stays plugged in, so Windows falls back to it whenever the TV
is switched off. Idle, it matches the TV: 3840x2160, 144 Hz (60 Hz if the driver does not offer 144),
225 % scale and one monitor. Windows then keeps the same logical desktop (1707x960), so windows keep
their size. Tests may change it while they run. `Restore-DisplayLayout` (the end of every `run.ps1`
virtual run, also on failure and on the pending path), `matrix.ps1` and `display.ps1 -Restore` put it back.
The values live in `$script:VirtualIdle` in `display.ps1`.

    pwsh -File tools\testenv\display.ps1 -Idle -DryRun      # what would change; changes nothing
    pwsh -File tools\testenv\display.ps1 -Idle              # apply it now
    pwsh -File tools\testenv\display.ps1 -SelfTest          # checks of the idle-state logic

Mode and scale can only be set while the virtual screen is on the desktop. Windows stores the scale
relative to the screen's recommended scale at that resolution (`DpiValue` under
`HKCU\Control Panel\Desktop\PerMonitorSettings\MTT1337...`), so the function sets it through the same
call as Settings and never writes that value by hand. Windows also remembers the mode it last used for
the "virtual screen only" layout. Run `-Idle` once while the TV is off (or let a nightly run end on the
pending path) to replace an old 1366x768 entry.

Setup, once per PC, as admin: `winget install VirtualDrivers.Virtual-Display-Driver`, then `devcon install`
of the package's `SignedDrivers\x86\VDD\MttVDD.inf` with hardware id `Root\MttVDD`. Create
`C:\VirtualDisplayDriver\vdd_settings.xml` first, with `<friendlyname>` set to the GPU Wind renders on,
and list the modes the matrix uses. List **3840x2160 first** with `<refresh_rate>144</refresh_rate>`:
the driver offers the first `<resolution>` as the preferred mode, which Windows uses when it falls back
to a screen it has no saved mode for. Keep 144 among the `<g_refresh_rate>` values, which the driver
adds to every resolution. The driver reads the file when it loads, so a change applies at the next
reload (a count change through the pipe, a reboot, or disabling and enabling the device).
Monitors appear with `MTT1337` in their device path.

What a virtual run proves: Wind starts, picks the right engine, zooms, stays within the cap and leaves
no stranded state on that resolution, refresh rate, scale and monitor count. It does not prove
smoothness or latency, so **virtual screens are health-only**: the absolute timing limits are calibrated
on the 144 Hz TV and do not apply. Nor is HDR covered. While the TV is on and attached as a secondary, DWM
still composes at its rate, so `hz` in the results is the screen's mode, not the pace measured.

## Baselines and CI

`baselines.json` stores per-scenario PASS numbers from the dev machine (`-UpdateBaseline`). `-CI`
compares with tolerance (1.6x + slack) and exits nonzero on regression. Absolute floors
(dtP99 > 25 ms, jitP95 > 25 px, backSteps in a non-rezoom scenario) catch catastrophes even
without a baseline.

The top-level numbers are the TV's. Each virtual screen keeps its own under
`profiles.<WxH@Hz,scale>`, so a virtual run is never judged against TV numbers; one without a baseline is
health-only until `-UpdateBaseline`. Record them with the TV off, so the screen paces itself.

GitHub-hosted runners cannot run this (no real GPU/display for Desktop Duplication or the
Magnification API), so the push-to-main story is local: `hooks/pre-push.sample` (reminder
gate) and `register_nightly.ps1` (scheduled full run on the dev machine).

## Files

| File | What it is |
|---|---|
| `run.ps1` | Orchestrator: suites (rapid, quick, full, soak, stress), scenarios, health checks, verdicts, JSON |
| `display.ps1` | Virtual screens: attach, make primary, set mode and scale, snapshot and restore the layout |
| `matrix.ps1` | Runs one suite across a list of virtual display setups, with one summary table |
| `bench.ps1` | Cross-magnifier benchmark: scoreboard and `results/catalog.csv` history |
| `lib.ps1` | Shared interop (injection, tones, Wind lifecycle, backdrops) and telemetry analysis |
| `backdrop.ps1` | One backdrop window per child process |
| `gamesim.cpp` | Borderless fullscreen D3D11 game simulator with a flip-model swapchain (build output `gamesim.exe` is gitignored) |
| `baselines.json` | Per-scenario PASS numbers from the dev machine |
| `sweep.ps1` | Runs a list of configurations through the gates, cheap gate first, and logs every result |
| `ab_acryl.ps1` | Round-robin multi-config run of the acrylic hitch scenarios (issue #229) |
| `cursor_area_probe.py` | Optical probe: cursor area and position per captured frame |
| `cursor_flicker.py` | Analyzer for the optical probe: does the cursor oscillate while panning |
| `shimmer_ab.ps1` | Ramp-shimmer A/B per configuration, hand still, blank backdrop |
| `shimmer_probe.py` | Measures cursor pixel churn while the zoom level changes |
| `probe_animated.ps1` | One-off probe of zoom engagement over the animated backdrop |
| `register_nightly.ps1` | Registers or removes the nightly full-suite scheduled task |
| `hooks/pre-push.sample` | Optional pre-push reminder to run the proving ground |

`results/` holds run output and is gitignored.
