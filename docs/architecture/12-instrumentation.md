# 12. Instrumentation and field method

Wind's output is compositor state on a real display and its input is a real mouse, so most
questions are settled by measurement on hardware. This chapter covers the logging built into the
binaries (`src/logging.*`), the diagnostic knobs compiled into Wind, and the scripts in `tools/`.

## Field method

- **Measure the artifact, not a proxy.** Write latency and write rate looked fine on three builds
  that still wobbled; the wobble is the cursor's deviation from the view centre during a pan, and
  `tools/mag_wobble_probe.ps1` measures exactly that.
- **Verify the experiment engaged.** A dead bind or a setting that did not apply fakes a clean
  result. Check the `txsession ... maxLevel=` log line before trusting a zoom run, and verify a
  control's backdrop or ini state before an A/B.

Write the conclusion into a findings file under `docs/` (index:
[README](README.md#docs-index)).

## The proving ground

`tools/testenv/` is the reusable automated environment: `run.ps1` restarts Wind with per-tick
telemetry (`src/test_telemetry.h`, `WIND_TESTLOG` or `%LOCALAPPDATA%\Wind\testlog.txt`), drives
scenarios (backdrop x zoom x movement) and compares pacing, ramp back-steps, cursor jitter and RAM
against `baselines.json`; `-CI` exits nonzero on regression. Suites run from ~1 to ~8 minutes.
Details: `tools/testenv/README.md`.

## Logging

Both binaries log through `src/logging.cpp`. The pure half (formatting, rotation, snapshot text) is
unit-tested; the Win32 half is excluded from the test build.

- **Files.** `%LOCALAPPDATA%\Wind\logs\` (`ResolveLogDir`): `wind-core.log` from Wind.exe,
  `wind-config.log` from WindConfig.exe. Rotation at 1 MiB over three generations. A second
  instance that cannot open the shared log writes `wind-core-<pid>.log`.
- **Lines.** `wind::Log(level, category, fmt, ...)` gives
  `2026-05-31T08:14:22.137Z  WARN  render  <msg>`. Thread-safe, flushes on Warn and Error.
  **Never log from the per-frame path**; hot paths log per-second summaries.
- **Startup snapshot.** Version and build flavour, OS build (`RtlGetVersion`), CPU, RAM, every
  adapter with driver version, every monitor with resolution, refresh, DPI and rotation, and the
  live config.
- **Crash dumps.** The unhandled-exception filter writes `wind-crash-<ts>.dmp` and a text summary
  (code, address, faulting module), heap-free from a directory resolved at `LogInit`.
- **Export diagnostics** (Settings > Preferences) zips the log folder to the Desktop. Files are
  stage-copied first, so the export never touches a live handle. The tray has no export.
- `diagnostics=1` adds the chattier frame-pacing trace in `%TEMP%\wind_diag.log`.

**Per-second and edge lines** (logged only when worth reading):

| Category | Source | Content |
|---|---|---|
| `txwrite` | `TransformModel::noteWrite` | Write count, avg/max ms, writes over 5 ms, failures; only when max > 5 ms or a write failed. A `fails` streak is the shared-runtime tell ([05](05-transform-engine.md)) |
| `ixwrite` | `TransformModel::noteIxWrite` | Input-transform publish cadence and timing, plus `stomps` |
| `txsession` | `TransformModel` | `session end maxLevel=..` per transform session |
| `cursor` | `RunTick`, `diagnostics=1` | Pointer distance from the lens centre at the weld instant. Blind to between-tick lag; use the wobble probes for that |
| `lock` | `RunTick` | Lock edges and the tell that caused them (`warp-anchor`, `seeded LOCKED at zoom-in`) |
| `hybrid` | `RunTick` | Engine pick per session |
| `snapshot` | `LogSystemSnapshot` | The startup block |

## Diagnostic knobs

Zero cost when off.

| Knob | Effect |
|---|---|
| `WIND_SELFTEST=1` | Renders headlessly and writes `wind_selftest.png`; the only way to capture the render overlay |
| `WIND_PACINGTEST=1` | Runs the render path at a forced cadence (proved the blt microstutter is DWM phase, not the loop) |
| `WIND_NOPARK=1` | Disables overlay parking for A/B |
| `WIND_NOHOOK` | Skips the LL hooks to exercise the polling fallback |
| `tdrTest` (hot) | Field harness: >0 forces Transform past the churny list; 2 probes the clamp; 4 lifts the MPO wall |
| `probeClicks` | 1 logs every coordinate space per click (Ctrl+click marks a dead spot); 2 adds a ~36 Hz pointer trace |
| `lockForce=1` (hot) | Forces the locked pan regime, to separate detector bugs from locked-path bugs |
| `txWobbleCage` | Draws bars around the cursor that flash on a screen-space wobble (`src/wobble_cage.*`) |
| `diagnostics=1` | Also logs render frame-build time apart from time blocked in `Present` (`RenderEngine::debugPerf`) |

## tools/

Standalone PowerShell scripts that call the same APIs Wind uses. `MagGetFullscreenTransform` reads
the one desktop magnification state whoever wrote it, so Wind and the built-in Magnifier are
directly comparable. Reading it needs `MagInitialize`, so a monitor script holds a context and pays
the cursor-change tax while it runs.

| Script | Measures |
|---|---|
| `mag_wobble_probe.ps1` | Cursor deviation from centre at a constant pan speed, Wind or built-in, same level |
| `mag_wobble_monitor.ps1` | Passive per-second log while a person drives: deviation, stale ms, backward writes, input-transform state |
| `mag_wobble_repro.ps1` | Scripted repro of a stale input transform left by the built-in Magnifier |
| `mag_perf_run.ps1` | Controlled pan, ramp, cycle, rezoom and zig-zag runs: compositor pacing, GPU, CPU, cadence (child: `gpu_sampler.ps1`) |
| `mag_ab_controlled.ps1` | Wind versus built-in on the same solid target, environment verified per run |
| `mag_latency_probe.ps1` | Cursor-move to transform-write latency (time-to-write, not photons) |
| `zoom_response_ab.ps1` | Zoom response as the user sees it, desktop or a named game |
| `pan_wake_probe.ps1` | Hitch on the first movement after a pause |
| `warm_cadence_sweep.ps1` | Scores `txWarmHz` values: pan-start hitch against GPU cost at rest |
| `gpu_ab.ps1` | dwm.exe and Wind.exe GPU per scenario, Wind versus built-in |
| `plane_race_probe.ps1` | Whether a session pulls a fullscreen game off its overlay plane |
| `wind_cadence_ab.ps1` | Wind's write cadence across ini configs, with `txwrite` timings |
| `wind_drive_probe.ps1` | Confirms injected side-button input reaches Wind's zoom bind |
| `magtrace.ps1` | Level, offsets and cadence of whichever magnifier is active |
| `flipwatch.ps1` | Presentation mode via PresentMon (independent flip versus composed) |
| `dwm_memprobe.ps1` | DWM memory from outside, surviving a dwm.exe crash |
| `gpu_breakdown.ps1`, `gpu_nvsmi.ps1` | Per-process GPU counters; whole-GPU load via nvidia-smi |
| `grid_window.ps1`, `acrylic_window.ps1`, `test_target_window.ps1` | Test targets: grid, real acrylic backdrop, solid control |

`PresentMon.exe` is not committed; `flipwatch.ps1` expects it in `tools/`. Build and release
scripts (`uiaccess_setup.ps1`, `installer_check.ps1`, `release.ps1`, `make_icon.mjs`) are covered
in [11](11-build-test-release.md).
