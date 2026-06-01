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

### Experiment 3 (last resort): Magnification-API low-power mode

A DWM-integrated path (what Windows Magnifier uses) is GPU-cheap by construction but reintroduces
integer-offset judder. Only if Experiments 1-2 cannot get the iGPU acceptable. Would ship as an
explicit opt-in "low-power / integrated-GPU" mode, never the default, preserving the smooth renderer
for capable hardware. Scoped to its own later spec if reached.

## Out of scope (for now)

Experiments 2 and 3 are contingent on Experiment 1's measurements - they are described here for
direction but each gets its own plan only if the measurements call for it. No default-behavior
change for capable hardware unless a measurement justifies it.

## Testing / verification

- Pure additions (`cursor_mapper::settled`, any dirty-state helper) get doctest unit tests.
- The render-gating and pacing are Win32 / loop behavior: verified by build + measurement.
- Primary verification is empirical: deploy to the iGPU machine, compare Task Manager GPU % during
  (a) static zoomed reading and (b) panning, before vs after. The new logger's snapshot already
  records the machine + GPU for correlating results.
- Regression check on the dev machine: pan/cursor smoothness and input latency unchanged; the
  `diagnostics=1` frame-pacing trace shows no new hitches when transitioning idle <-> active.
