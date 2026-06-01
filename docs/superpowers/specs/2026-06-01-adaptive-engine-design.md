# Adaptive Engine (auto low-power) Design

**Status:** Approved (user chose adaptive auto-switching; delegated decisions). Issue #83. Branch `perf/gpu-reduction`.
**Date:** 2026-06-01

## Goal

On a single machine, be as cheap as Windows Magnifier on the desktop AND keep fullscreen games at
full FPS - by automatically choosing the magnifier engine from what is in the foreground.

## Background (measured)

Two engines, each better for one workload (confirmed on hardware):
- **Low-power (Magnification API, `MagSetFullscreenTransform`)**: ~2-3% GPU on the desktop, but it
  forces DWM to composite + scale a fullscreen game on its present path, kicking the game off its
  direct-scanout fast path - game FPS drops (~144 -> ~70 when panning zoomed).
- **Own-renderer (DXGI Desktop Duplication + D3D + layered overlay)**: keeps a zoomed game at full
  FPS (it captures + magnifies in a separate overlay, off the game's present path), but its overlay
  + capture cost ~60% GPU on a weak iGPU desktop.

Neither is universally best. `lowPower=1` (always Mag API) throttles games; `lowPower=0` (always
own-renderer) is heavy on an iGPU desktop. Adaptive picks per-workload.

## Config

Extend `lowPower` from a 0/1 flag to a mode:
- `0` = own-renderer always (default; capable/gaming machine - smooth + games at full FPS).
- `1` = low-power (Mag API) always (weak desktop machine that never games).
- `2` = **auto**: low-power on the desktop, own-renderer when a fullscreen game/app is foreground.

Default stays `0`. Per-machine (the ini is per-machine in `%LOCALAPPDATA%`).

## Detection: "is a fullscreen game/app in the foreground?"

A small `foreground_state` helper (Win32) returns true when the own-renderer should be used:
- **Primary signal:** `SHQueryUserNotificationState()` returns `QUNS_RUNNING_D3D_FULL_SCREEN` or
  `QUNS_PRESENTATION_MODE` (Windows' own fullscreen-app detector, used by Focus Assist). Catches
  exclusive-fullscreen DX games.
- **Fallback for borderless-fullscreen games:** the foreground window's rect exactly covers (or
  exceeds) its monitor's bounds AND it is not the shell/desktop (`Progman`/`WorkerW`, the taskbar,
  or our own windows). Catches borderless games that the primary signal misses.
- **Bias when uncertain toward the own-renderer.** A false positive (a maximized window or fullscreen
  video treated as a game) only costs some desktop GPU; a false negative (a real game missed) drops
  the game to ~70 FPS, which is the worse failure. So the fallback errs toward "use the own-renderer."

Polled at a low rate (~2 Hz), NOT per frame - foreground/fullscreen state changes are human-scale.

## Switch policy: only at 1x, lazy init/teardown

The expensive own-renderer pipeline (overlay + Desktop Duplication) must NOT exist while idle on the
desktop (that is the ~60% cost we are avoiding), so the engines are initialized/torn down lazily and
only ONE is live at a time:

- A background re-evaluation (the ~2 Hz poll) sets a `desiredEngine` (Render when a fullscreen app is
  detected, Mag otherwise).
- The actual switch happens ONLY when the magnifier is at 1x (not zoomed): tear down the active
  engine, initialize the desired one, make it active. If `desiredEngine` changes while zoomed, the
  switch is deferred until the next zoom-out. This guarantees the engine never swaps mid-view (no
  glitch) and the heavy own-renderer only exists while a game is actually running.
- At startup, evaluate once and initialize the matching engine.

Net behavior: alt-tab into a game, and the next zoom uses the own-renderer (game stays 144); close
the game, the own-renderer is torn down and the desktop returns to ~2-3%.

## Components

- **`src/foreground_state.{h,cpp}`** (new, Win32): `bool FullscreenAppForeground()` implementing the
  detection above. Small and isolated. (A pure helper for the "rect covers monitor" comparison can be
  unit-tested; the Win32 queries are inspection-verified.)
- **`src/render_engine`**: verify (and fix if needed) that `initialize()` works after a prior
  `shutdown()` in the same process - adaptive tears down and rebuilds it when games start/stop. This
  is the #1 implementation risk and is de-risked first (a standalone init->shutdown->init->render
  harness).
- **`src/main.cpp`**: `TickState` holds both engines and an `ActiveEngine { Render, Mag }` + the
  adaptive flag + `lastEngineCheck` timer; a `selectEngine()` step (poll-rate-limited, 1x-gated) does
  the teardown/init switch; `RunTick` dispatches the zoomed work to the active engine. `wWinMain`
  initializes the startup engine per the mode and detection.
- **`src/config.{h,cpp}`**: `lowPower` accepts 0/1/2 (clamp to that range); ini comment updated.

## Risks

1. **RenderEngine re-init after shutdown.** It was built to initialize once. Adaptive needs
   init -> shutdown -> init to work cleanly (recreate the window, swapchain, DDA, D3D). De-risked
   first with a standalone harness; if it does not work, fix RenderEngine's shutdown/initialize to be
   re-entrant before wiring adaptive.
2. **Detection accuracy** (esp. borderless games). Mitigated by the own-renderer bias and by being a
   low-rate poll the user can observe. Tunable.
3. **Switch latency.** Initializing the own-renderer takes tens of ms; it happens at 1x (not while
   zoomed) when a game is detected, so it is off the zoom-in critical path in the common case
   (detected before the user zooms). Acceptable.

## Out of scope

- Switching engines WHILE zoomed (deferred to zoom-out - simpler and glitch-free).
- Multi-monitor in low-power mode (already a low-power limitation; unchanged).
- Driving low-power panning from raw input when a game locks the cursor (low-power follows the OS
  cursor; in a fullscreen game the adaptive mode is on the own-renderer anyway, which handles locked
  cursors).

## Testing / verification

- Pure: the "foreground rect covers monitor" comparison helper gets a doctest.
- Win32 (inspection + harness): the RenderEngine init->shutdown->init->render harness (risk 1); the
  detection helper checked against a real fullscreen app vs the desktop.
- Empirical (the real verdict), on the iGPU machine with `lowPower=2`:
  - Desktop zoomed reading: ~2-3% GPU (low-power active).
  - Zoomed inside a fullscreen game: FPS stays ~144 (own-renderer active), and on game exit the
    desktop returns to ~2-3% (own-renderer torn down).
  - No glitch/flash when alt-tabbing between desktop and a game (engine swaps only at 1x).
- Regression: `lowPower=0` (default) unchanged; `lowPower=1` unchanged.

Plan: `docs/superpowers/plans/2026-06-01-adaptive-engine.md`.
