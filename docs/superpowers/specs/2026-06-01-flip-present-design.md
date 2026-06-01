# Flip-Model (DirectComposition) Present - opt-in cheap own-renderer

**Status:** Approved (user chose option B; delegated). Issue #83. Branch `perf/gpu-reduction`.
**Date:** 2026-06-01

## Goal

Let the OWN-renderer (sub-pixel smooth, app-drawn cursor - locked + smooth) run cheap on a weak
integrated GPU by presenting its overlay via a DirectComposition flip-model swapchain that the
display can scan out on an independent-flip / MPO plane, instead of the blt-model swapchain that DWM
must composite every frame (the ~60% iGPU cost). Strictly opt-in, never the default.

## Why this is the only path to locked + smooth + cheap

The Magnification API can't do sub-pixel positioning, so it can never give smooth content (it steps
in whole pixels) - only the own-renderer is smooth. The own-renderer's cost is DWM compositing its
fullscreen layered surface every frame. A flip-model present (independent flip / MPO) hands the
surface to the display controller to scan out directly, removing that composite cost - while the
own-renderer keeps its sub-pixel sampling and app-drawn cursor (so locked + smooth are preserved).

## Why it was removed, and why opt-in is safe

The dcomp flip path was implemented and removed in #69 (`c44dc72`): on a VRR / G-Sync display the
independent-flip plane scans out unsynced and TEARS on frame hitches (confirmed 1:1 with loop
hitches). That is a VRR-specific failure. On a FIXED-REFRESH display the plane scans out at the
panel's steady refresh (vblank-aligned) and does not tear. So:
- **Default stays blt** (`flipPresent=0`). The CLAUDE.md "do not re-attempt dcomp" gotcha holds for
  the default / VRR case - unchanged.
- **`flipPresent=1` is opt-in, per-machine** (the ini is per-machine). The user sets it ONLY on the
  fixed-refresh iGPU. On a VRR machine they leave it 0. Wind also logs a clear warning when it is on
  (tears on VRR), and the option is documented as fixed-refresh-only.

Auto-detecting VRR reliably is not feasible from a public API, so this relies on the user enabling
it only where appropriate (they know which monitor is VRR). Default-off means it can never affect a
machine the user didn't opt in.

## Design

- **Recover the #69 dcomp present** from `c44dc72^` (`src/render_engine.cpp`): the flip-model
  swapchain (`CreateSwapChainForComposition` / `IDXGISwapChain1`), the `IDCompositionDevice` /
  `IDCompositionTarget` / `IDCompositionVisual` on the same layered click-through HWND, and the
  present-via-flip-swapchain path. We do NOT recover the full dual-swapchain / `setPresentMode` /
  `PresentPolicy` adaptive machinery (#69's runtime blt<->dcomp switching) - that is overkill. We
  recover a single selectable present backend chosen once at `buildPresent()`.
- **Config:** add `int flipPresent = 0;` (default 0 = blt; 1 = dcomp flip-model). Clamp 0/1.
- **`render_engine` API:** `initialize()` gains a `flipPresent` argument (or reads it via a setter
  before init). `buildPresent()` builds the dcomp flip swapchain + visual when `flipPresent`, else
  the existing blt swapchain. `renderFrame`'s `Present` targets whichever swapchain is active.
  `releasePresent()` / `shutdown()` release the dcomp objects too (needed for the re-entrancy the
  adaptive engine relies on).
- **main.cpp:** pass `cfg.flipPresent` into `renderEngine.initialize(...)` (and the adaptive
  re-init in `SelectEngine`). No other tick changes - the own-renderer path is otherwise identical.
- **Logging:** the startup snapshot already records the machine; add a one-line `Log` noting
  `flipPresent` is on (and the VRR caveat) so a customer's log shows it.

## Risks / uncertainty (be explicit)

1. **MPO promotion is not guaranteed.** The GPU win depends on the driver giving our layered+dcomp
   fullscreen window an independent-flip / MPO plane. If it instead composites it through DWM
   anyway, there is NO GPU win (the experiment fails on that hardware). Only measuring on the iGPU
   tells us. Acceptable - it is opt-in and falls back to blt if disabled.
2. **Tearing on VRR** (the #69 reason) - mitigated by default-off + opt-in only on fixed-refresh +
   the logged warning.
3. **Re-entrancy:** the dcomp objects must be fully released in `shutdown()` so the adaptive engine's
   teardown/re-init (already shipped) still works. Covered by recovering `releasePresent`'s dcomp
   teardown.

## Verification (the verdict is on the iGPU)

- Build all targets; `flipPresent=0` (default) must be byte-for-byte the current blt behavior
  (regression check on the dev/VRR machine - no tearing, unchanged).
- On the fixed-refresh iGPU with `lowPower=0` + `flipPresent=1`: measure zoomed GPU % (does it drop
  toward the MPO/no-composite ideal?) and confirm NO tearing and the pan stays smooth + cursor
  locked. If GPU drops and no tear -> success (keep it). If no GPU drop -> the driver isn't giving
  an MPO plane; the experiment failed on that hardware (revert / keep blt).
- RTSS tell: over blt the RTSS overlay shows (composited); over a true independent-flip plane it
  vanishes - a quick confirmation of whether we got the MPO plane.

Plan: `docs/superpowers/plans/2026-06-01-flip-present.md`.
