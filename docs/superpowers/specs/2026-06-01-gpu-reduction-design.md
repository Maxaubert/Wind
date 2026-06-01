# GPU Usage Reduction Design

**Status:** Approved (autonomous - user delegated decisions). Issue #83. Branch `perf/gpu-reduction`.
**Date:** 2026-06-01

## Goal

Cut the magnifier's GPU usage toward Windows Magnifier levels (well under 10% on integrated
graphics), WITHOUT losing the sub-pixel pan/cursor smoothness the own-renderer was built for.
Today, static zoomed reading spikes an iGPU from ~3% to ~60% (and ~0% to ~8% on a discrete GPU).

## Root cause

Two facts from the code:
1. **Unconditional full-rate redraw while zoomed.** `RunTick` (`src/main.cpp:317`) calls
   `renderFrame` every tick with no dirty-check, and while zoomed the loop is paced by `Present(1,0)`
   to the display refresh (`src/main.cpp:729`). So even when the desktop is static, the hand is
   still, and the zoom has settled, we capture + magnify + composite a fullscreen layered overlay at
   the full refresh rate (144 Hz on the dev machine). Windows Magnifier only does GPU work on change.
2. **The cost and the smoothness share a cause.** The own-renderer (DDA capture + D3D11 + layered
   blt overlay) exists because the public Magnification API exposes only an integer pixel offset
   (`MagSetFullscreenTransform`), which judders worse with zoom
   (`docs/superpowers/specs/2026-05-25-own-renderer-design.md`). The smoothness we want and the
   per-frame GPU cost both come from rendering ourselves.

The dominant pain (reading) is the static case, where redrawing at all is pure waste.

## Approach: experiment-driven, on one branch

Empirical effort. Implement each experiment, measure GPU before/after on the iGPU machine (Task
Manager GPU %), keep what works. Ordered cheapest/highest-leverage first.

### Experiment 1 (primary): render-on-demand

Only run the GPU-expensive work (desktop copy + magnify pass + Present/overlay composite) when
something actually changed. Keep the full own-renderer otherwise untouched, so quality is preserved.

**Dirty condition** - a tick presents a new frame only if ANY of:
- The desktop content changed (Desktop Duplication delivered a new frame this tick).
- The lens panned (pan delta `dx != 0 || dy != 0`).
- The smoothed lens center has not yet settled on its target (cursor-smoothing inertia still
  converging, so the view keeps moving for a few frames after input stops).
- The zoom level is animating (level changed since last tick, or zoom direction is non-neutral).
- A forced refresh: the zoom-in reveal frame, a monitor retarget, a config hot-reload, a
  cursor-visibility toggle, or device-lost recovery.

When none hold, skip the magnify + Present entirely and pace the idle wait on the existing waitable
timer instead of on vsync.

**Why this is cheap when idle.** Desktop-change detection stays via `AcquireNextFrame(0)`, which on
a static screen returns `WAIT_TIMEOUT` immediately with no GPU copy. So an idle tick costs only a
cheap DDA poll + `GetCursorPos` (CPU, negligible) - no capture copy, no magnify draw, no fullscreen
composite. During static reading the GPU drops to near idle, matching Windows Magnifier.

**Games / moving content stay correct.** A game or video changes the desktop and/or moves the cursor
every frame, so the dirty condition is true every frame and we render at full rate - which is what
those cases need. Only genuinely-static viewing goes idle.

**Pacing.** While zoomed: if we presented this tick, `Present(1,0)` paces (as today); if we skipped,
the waitable timer paces the idle poll (reuse the existing idle/1x timer path). First-input latency
is bounded by the idle poll interval (refresh-rate poll keeps it imperceptible).

**Components touched.**
- `src/cursor_mapper` (pure): add a `settled()` predicate (rendered center within an epsilon of the
  target) so the loop knows when smoothing has converged. Unit-tested.
- `src/render_engine`: `capture()` reports whether it copied a new frame; the magnify + Present run
  only when the caller asks (dirty). A small API change so `renderFrame` can be split into
  capture-and-detect vs present, or gated by a force/dirty flag.
- `src/main.cpp` `RunTick` + the main loop: compute the dirty condition, call the gated render, and
  choose vsync-vs-timer pacing based on whether a present happened.

**Success metric.** iGPU GPU % during static zoomed reading drops from ~60% to single digits
(near the idle baseline / Windows Magnifier). Panning measured separately (Experiment 2 territory).
No regression to pan/cursor smoothness or input latency on the dev machine.

### Experiment 2 (if panning is still heavy on the iGPU): low-power knobs

Panning genuinely needs redraws, so Experiment 1 does not help it. If continuous panning still
pegs the iGPU:
- Cap the zoomed present rate (e.g. 60 Hz instead of 144) - halves the panning cost on a high-refresh
  panel for imperceptible smoothness loss.
- Re-enable `cropCapture` (default off since the edge-staleness fix) under a low-power profile, to
  cut the 4K/HDR desktop copy by ~zoom^2 while panning.
Bundle these behind a `lowPower` config profile rather than changing the default, so the dev
machine's quality is untouched.

### MEASUREMENT (2026-06-01): Experiment 1 did not move the needle

Deployed render-on-demand to the iGPU machine. Static zoomed reading with the mouse held perfectly
still still pegs the iGPU at ~60% - only ~1 percentage point better than before. Since
render-on-demand provably eliminates our redraw work when idle (the magnify pass, the desktop copy,
and Present all skip - reviewed and traced), and idle GPU is essentially unchanged, **our redraw was
never the cost.** The cost is architectural and runs whether or not we present:

1. **The fullscreen layered overlay.** It is a 4K `WS_EX_LAYERED` alpha surface that DWM must blend
   into every screen composite (driven by any on-screen change), regardless of our Present.
2. **The live Desktop Duplication session**, which keeps the GPU producing frames and can block iGPU
   power-saving.

Windows Magnifier has neither - it scales inside DWM. So Experiment 2 (fps cap / cropCapture) is
also moot: those reduce per-present cost, but the cost is present even with zero presents.
Render-on-demand is kept anyway (correct, no regression, trims work/power on truly static screens),
but the real fix is Experiment 3.

### Experiment 3 (ACTIVE): Magnification-API low-power mode

The only way to reach Windows-Magnifier GPU cost is the path Windows Magnifier itself uses: the
public Magnification API (`MagSetFullscreenTransform`), where DWM does the scaling - no overlay
surface, no Desktop Duplication. GPU-cheap by construction. The tradeoff is integer pixel offsets
(`xOffset`/`yOffset`), so the pan judders (worse with zoom); this is exactly why the engine was
removed in issue #20. Accepted for a low-power mode.

**Design.**
- Reintroduce `src/magnifier_engine.{h,cpp}` (recover verbatim from git `969c952^`, the commit before
  its #20 removal). It is ~50 lines: `initialize()` = `MagInitialize`; `setTransform(level, xOffset,
  yOffset)` = `MagSetFullscreenTransform` + `MagSetInputTransform` (maps clicks; needs UIAccess,
  which the deployed build has, and is a harmless no-op otherwise); `shutdown()` resets to 1x and
  `MagUninitialize`. It is Win32 (uses `<windows.h>`), so excluded from the pure test build.
- Add a `lowPower` config flag (default 0). The ini lives per-machine in `%LOCALAPPDATA%`, so the
  school iGPU sets `lowPower=1` while the dev machine stays 0 (smooth own-renderer untouched).
- In `main.cpp`, when `lowPower=1`, drive `MagnifierEngine` instead of `RenderEngine`: do NOT create
  the overlay / Desktop Duplication / D3D device at all; do NOT hide the OS cursor (the Mag API
  magnifies the real cursor). Each zoomed tick, compute the integer source offset that centers the
  cursor - `xOffset = clamp(round(cursorX - (sw/level)/2), 0, sw - sw/level)`, same for y - and call
  `setTransform(level, xOffset, yOffset)`. On zoom-out, `setTransform(1.0, 0, 0)`. The zoom ramp,
  hold-to-zoom input, and config hot-reload all stay as-is; only the per-tick "draw" call differs.
- The render-on-demand idle gating is irrelevant in this mode (the Mag API does not redraw); it
  remains for the own-renderer default path.

**Success metric.** With `lowPower=1` on the iGPU, zoomed GPU usage (static AND panning) drops to
Windows-Magnifier levels (well under 10%). Pan will judder (integer offset) - acceptable for the
low-power mode; the smooth own-renderer remains the default for capable hardware.

This experiment gets its own implementation plan: `docs/superpowers/plans/2026-06-01-lowpower-mag-engine.md`.

## Out of scope

No default-behavior change for capable hardware: `lowPower` defaults to 0 and the own-renderer
remains the default. Games-while-locked panning in low-power mode (driving the offset from raw input
when a game clips the cursor) is deferred - the low-power mode follows the OS cursor, which suits its
target (desktop reading on weak/integrated GPUs).

## Testing / verification

- Pure additions (`cursor_mapper::settled`, any dirty-state helper) get doctest unit tests.
- The render-gating and pacing are Win32 / loop behavior: verified by build + measurement.
- Primary verification is empirical: deploy to the iGPU machine, compare Task Manager GPU % during
  (a) static zoomed reading and (b) panning, before vs after. The new logger's snapshot already
  records the machine + GPU for correlating results.
- Regression check on the dev machine: pan/cursor smoothness and input latency unchanged; the
  `diagnostics=1` frame-pacing trace shows no new hitches when transitioning idle <-> active.
