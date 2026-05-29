# DComp present spike: data-driven decision on flip-model vs blt+DwmFlush

Date: 2026-05-29
Issue: [#69](https://github.com/Maxaubert/Wind/issues/69)
Status: design approved, pre-implementation

## Problem

The in-zoom smoothness ceiling is the blt-model swapchain plus the `DwmFlush()` pacing
workaround it forces. A flip-model swapchain (via DirectComposition) would compose without that
phase mismatch, but `WS_EX_LAYERED` (required for cross-process click-through) cannot use
flip-model, so the overlay is stuck on blt.

This has been attempted twice and reverted, and the prior verdicts were unreliable:

- **PR #11 (`1ea58b7`, closed).** Dropped `WS_EX_LAYERED`, used
  `WS_EX_NOREDIRECTIONBITMAP | WS_EX_TRANSPARENT` + DComp flip-model. Zoom smoothness was
  excellent, but real cross-process clicks were eaten. Its click-through "proof"
  (`tools/dcomp_clickthrough_test.cpp`) used `WindowFromPoint`, which only does geometric
  hit-testing and returns the window beneath even when actual mouse-message delivery to another
  process still fails. **False positive. This is the single most important bug to fix this time.**
- **PR #24 / #25 (`d93408a`, merged then reverted in #28 `a477605`).** Kept `WS_EX_LAYERED` AND
  added DComp flip-model on the same window; proved the two coexist, so click-through AND smooth
  presentation both worked. Reverted because A/B testing pinned background-game lag on the dcomp
  backend, "for no perceptible upside." Both the regression and the no-upside verdict were
  **by feel, never measured** (no `WIND_PACINGTEST` numbers for dcomp).

So both prior failures came from (a) a broken click-through test and (b) unmeasured,
subjective performance verdicts. This spike fixes both before any migration.

## Goal

Decide, **by measurement**, whether a DComp flip-model overlay can replace blt+DwmFlush and
improve in-zoom smoothness without (a) breaking cross-process click-through or (b) regressing
background-app responsiveness. Migrate the engine only if a config clears all gates; otherwise
document the dead-end definitively and close #69.

## Non-goals

- No engine migration in this phase. That is a separate, gated Phase 2 plan.
- No changes to the capture / magnify / cursor pipeline. Only the window + present path.
- Not the small perf items (#70-74); those are tracked and landed separately.

## Architecture

A standalone harness under `tools/present_spike/`, built independently of `Wind.exe`, in three
pieces. Keeping it out of the engine isolates the experiment and avoids destabilizing the
shipping render path while we measure.

### 1. `clickprobe.cpp` - separate-process click target

A tiny visible window with a "button" rect at a known screen location. Logs a QPC timestamp on
every `WM_LBUTTONDOWN` / `WM_LBUTTONUP` it receives, to a results file. Running as its own
process is what makes the click-through test valid (the #11 same-thread caveat does not apply).

- The pass/fail criterion for click-through is **"did this other process receive the click"**,
  read from the probe's log. `WindowFromPoint` may be logged for information but is never the
  criterion.
- The probe also serves as the latency target (below): it records receipt QPC so the harness can
  compute injection-to-receipt time.

### 2. `harness.cpp` - the overlay under test

Creates a fullscreen overlay in a config selected by command-line argument:

- `blt` - the current path: blt-model `DXGI_SWAP_EFFECT_DISCARD` on a `WS_EX_LAYERED` window,
  paced by `DwmFlush()`. The baseline.
- `dcomp-nolayer` - the #11 config: `WS_EX_NOREDIRECTIONBITMAP | WS_EX_TRANSPARENT` (no layer),
  `CreateSwapChainForComposition` flip-model presented via a DComp device/target/visual.
- `dcomp-layered` - the #24 config: same DComp flip-model but on a `WS_EX_LAYERED` window.

It renders a recognizable semi-transparent frame (e.g. a moving gradient) so display and
per-pixel transparency are visually confirmable and the probe beneath is visible. It runs three
automated checks per config:

- **Click-through.** Place the probe beneath the overlay, `SendInput` a click at the probe's
  button rect, confirm delivery via the probe's log. Reports PASS/FAIL. Repeat for left button,
  and a down-move-up drag, to catch partial delivery.
- **Pacing.** Reuse the existing `WIND_PACINGTEST` loop logic (simulated pan, interval stats):
  log avg dt, max dt, hitches > 1.5x, big > 2.5x per config. This is the smoothness measurement.
- **Background-latency probe.** `SendInput` N timestamped clicks to the probe and record
  (probe-receipt QPC - injection QPC) per click, with each overlay config present vs a
  no-overlay baseline. Report median and p95. This is the objective proxy for the
  "background-game lag" that got #24 reverted.

### 3. Runbook + results doc

A short `tools/present_spike/README.md`: build line, the commands to run each config, and where
the logs land. Includes the steps the user runs under the real 4K-HDR-over-a-game workload
(launch a game, run the harness against each config, paste the logs back).

## Decision gate

Thresholds are tunable, but the structure is fixed. A DComp config is viable only if it clears
all of these against the `blt` baseline:

1. **Click-through: MUST pass** (probe receives the click). A config that fails here is dead,
   full stop.
2. **Pacing: measurable improvement.** Lower max dt and/or fewer hitches than blt+DwmFlush in
   `WIND_PACINGTEST`. If there is no smoothness upside, there is no reason to migrate.
3. **Background latency: no meaningful regression.** Median injection-to-receipt latency on the
   background probe within ~1 frame (~7 ms at 144 Hz) of the blt baseline.
4. **Subjective confirmation under a real game** by the user, now backed by the latency numbers
   (the exact thing #28 reverted on, no longer judged by feel alone).

If more than one config passes, prefer `dcomp-nolayer` (cleanest, no layered/redirection
surface) over `dcomp-layered`.

The winning config feeds a **Phase 2 migration plan** (separate, via writing-plans) that
resurrects `d93408a`'s `present=dcomp` code into `render_engine` behind a `present=` config knob,
A/Bs it in the real app, then makes it the sole path and deletes blt if it wins. If no config
passes, write the negative result into CLAUDE.md and close #69.

## Division of labor

- Automatable on the dev machine (no game needed), run by Claude: the click-through matrix and
  the pacing test for all three configs.
- Needs the real rig, run by the user: background-game responsiveness under real 4K HDR. Claude
  provides the harness + a one-line runbook; the user pastes the logs.

## Invariants that must hold for any candidate (from CLAUDE.md)

- `WDA_EXCLUDEFROMCAPTURE` on the overlay, or Desktop Duplication captures our own presented
  frame and we magnify our own output (feedback loop -> black).
- Cross-process click-through (the entire point of the spike).
- Reveal-after-present and the alt-tab no-flash behavior (DComp's per-pixel-alpha show/hide path
  already addresses this; verify it is preserved).
- Stay above always-on-top app overlays (RTSS, Task Manager); re-assert topmost as today.

## Risks

- **Non-layer click-through may be impossible** on this Windows constraint. If so, the only
  viable DComp config is `dcomp-layered`, and the whole decision rests on the latency probe.
- **The latency probe is a proxy.** Felt "lag" may not fully reduce to injection-to-receipt
  time; it is the weakest measurement, so the user's subjective check stays in the loop.
- **DComp on a layered window is a hybrid** that may itself be the cause of the #24 background
  lag; the probe should make that visible rather than us guessing.
