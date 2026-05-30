# Dual-swapchain instant present switching (issue #69)

Date: 2026-05-29
Issue: [#69](https://github.com/Maxaubert/Wind/issues/69)
Builds on: the dcomp present path + present=auto specs (same date)
Feasibility: proven by `tools/present_spike/dualswap.cpp` (commit f0b1291)
Status: design approved, pre-implementation

## Problem

`present=auto` correctly detects the dcomp throttle (windowed app over a background game), but it
could not apply the switch usefully: switching present modes meant rebuilding the swapchain, which
is only safe at a zoom boundary (overlay hidden). With hold-to-zoom, a continuous hold never
crosses a boundary, so it stayed laggy; and a mid-zoom rebuild (tried) broke the invariants - it
reset the zoom and corrupted the layered-window visibility (zoom-out left a frozen frame).

The `dualswap` spike proved the fix: a blt-model redirection swapchain and a DComp flip-model
visual **coexist on one overlay HWND**, and which one displays can be flipped **instantly** via
`IDCompositionVisual::SetContent(swapFlip)` vs `SetContent(nullptr)` + `Commit` - no rebuild. That
makes a present-mode switch a cheap, hitch-free, zoom-preserving operation that can run mid-zoom.

## Goal

Re-architect the engine present path to keep **both** swapchains alive and switch by flipping the
DComp visual content, so `present=auto` switches blt<->dcomp seamlessly any time (including
mid-hold) with no blip, no zoom reset, and no visibility corruption.

## Non-goals

- No change to the capture / magnify / cursor pipeline.
- No change to the `present=auto` detection policy (`PresentPolicy` is unchanged); only how/when
  its choice is applied.
- Not removing `present=blt` / `present=dcomp` as forceable fixed modes.

## Architecture

The overlay window is unchanged (one fullscreen layered, transparent, click-through,
capture-excluded HWND). The present path inside `render_engine.cpp` changes from "one swapchain,
rebuilt on mode change" to "both swapchains live, flip the DComp visual."

### Components (`render_engine.cpp` State)

Both present paths exist simultaneously (already members from prior work, now both populated):
- blt: `ComPtr<IDXGISwapChain> swap` (DISCARD, on the HWND - redirection).
- dcomp: `ComPtr<IDXGISwapChain1> swapFlip`, `IDCompositionDevice dcomp`,
  `IDCompositionTarget dcompTarget`, `IDCompositionVisual dcompVisual`.
- `PresentMode present` = which is currently shown.
- The RTV is the active swapchain's back buffer (flip: re-acquired each frame; blt: stable).

### buildPresent() - create BOTH

Replace the either/or `buildPresent()` with one that creates both paths once:
1. blt swapchain on the HWND + its RTV (as today's blt branch).
2. flip swapchain via `CreateSwapChainForComposition` + `DCompositionCreateDevice(dxgiDev)` +
   `CreateTargetForHwnd(hwnd, TRUE)` + `CreateVisual` + `SetRoot`.
3. Set the DComp visual content to match the initial `present`: `SetContent(swapFlip)` for dcomp,
   `SetContent(nullptr)` for blt; `Commit`.
4. `SetLayeredWindowAttributes(hwnd, 0, 0, LWA_ALPHA)` (start hidden; visibility is the unified
   `LWA_ALPHA` toggle, see below).

### setPresentMode(mode) - instant flip, no rebuild

If `mode == present` return true. Otherwise:
1. Render + present a fresh frame to the newly-active swapchain so it is not stale (the caller may
   instead let the next `renderFrame` do this; to avoid a one-frame stale flash on a visible
   switch, present a current frame here before committing the visual).
2. `dcompVisual->SetContent(mode == Dcomp ? swapFlip : nullptr)`; `dcomp->Commit()`.
3. `present = mode`. Force a fresh capture (`dupl.Reset(); haveDesktop = false; freshCapture = true`)
   so the next zoomed frame is live. Return true.

This never releases/rebuilds a swapchain, so it is hitch-free and cannot touch zoom or break
visibility. It is safe to call at any time, zoomed or not.

### renderFrame - present the active swapchain

Acquire the active back buffer's RTV (flip: re-acquire buffer 0 each frame; blt: stable), run the
existing render(), then present the active swapchain: `swapFlip->Present(p.vsync?1:0,0)` for dcomp,
`swap->Present(p.vsync?1:0,0)` for blt. (Loop-level `DwmFlush` for blt+dwmFlush unchanged.)

### Unified visibility (LWA_ALPHA for both modes)

Show/hide the overlay (zoom in/out) by toggling the layered window's constant alpha - the same for
**both** modes, because the DComp visual is composited within the layered window:
- `setVisible(false)` -> `SetLayeredWindowAttributes(hwnd, 0, 0, LWA_ALPHA)`.
- `setVisible(true)`  -> present the live frame first (caller does), then
  `SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA)` + re-assert `HWND_TOPMOST`.

This unifies the two previously-divergent hide models (blt alpha vs dcomp transparent-present) and
removes a class of visibility bugs. The window stays shown the whole time (alt-tab no-flash holds).

IMPLEMENTATION RISK (verify, with fallback): confirm `LWA_ALPHA 0` actually hides the DComp visual
(the dualswap spike only exercised alpha 255). If alpha-0 does NOT hide the DComp visual, fall back
to a per-mode hide: blt via `LWA_ALPHA`, dcomp via presenting a fully-transparent frame (the prior
`presentTransparent` approach). The plan must include this verification step before relying on the
unified model.

### main.cpp - apply the policy's choice immediately

Because switching is now free and mid-zoom-safe, drop the zoom-boundary gating for the auto policy:
apply `desiredPresent` via `setPresentMode` the moment it differs from `renderEngine.presentMode()`,
every tick, regardless of zoom level. (Fixed `present=blt`/`dcomp` still just pin `desiredPresent`.)
Remove the now-unnecessary at-1x / zoom-in special-case apply blocks; a single per-tick apply
suffices. Keep the diagnostics log of each switch + reason.

### retarget / shutdown

- `retarget` (monitor change): `ResizeBuffers` on BOTH swapchains, re-acquire both RTVs (or the
  active RTV plus mark the other for re-acquire on its next use). Keep the multi-GPU guard.
- `shutdown`/`releasePresent`: release both swapchains + the dcomp objects.

## Invariants preserved

- Cross-process click-through (one layered transparent window, unchanged).
- `WDA_EXCLUDEFROMCAPTURE`, per-frame topmost re-assert, HDR FP16 path, cursor pass - unchanged.
- Alt-tab no-flash (window stays shown; alpha-toggle visibility; present-then-reveal).
- Zoom state lives outside the engine and is never touched by a present switch (by construction).
- No mid-zoom rebuild (the source of the earlier zoom-reset + frozen-frame bugs) - we flip the
  visual instead.

## Error handling

- `buildPresent` failure (either path): fail `initialize` (as today); the engine needs both.
- A `SetContent`/`Commit` failure in `setPresentMode`: log via `RLog`, keep the previous `present`
  (do not leave the visual in an inconsistent state), return false; the policy retries next tick.

## Testing / verification

- `WIND_SELFTEST` dumps a correct magnified PNG in `present=blt` and `present=dcomp` (forced),
  including HDR. (The dump reads the active swapchain - already handled.)
- `build.bat test` green; `build.bat check` clean. `PresentPolicy` doctests unchanged and passing.
- The `dualswap` LWA-hide question is settled in the plan via a quick check (extend the spike or a
  one-off) before the unified-hide code is relied on.
- User in-game (`present=auto`, `diagnostics=1`): a continuous hold in the bad state now switches
  to blt **mid-hold** with no blip, no zoom reset, no frozen frame on zoom-out; game-foreground
  stays dcomp; the diag log shows `present: -> blt (throttle)` / `-> dcomp (cue)` switches.

## Follow-ups (out of scope)

- Record the dual-swapchain present architecture + the throttle gotcha in CLAUDE.md.
- Once validated in-game, advance/close #69; reassess whether blt's `dwmFlush` workaround is still
  needed now that auto covers the bad state.
