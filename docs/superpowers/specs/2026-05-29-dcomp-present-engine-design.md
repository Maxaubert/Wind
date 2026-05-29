# DComp flip-model present path in render_engine (Phase 2, issue #69)

Date: 2026-05-29
Issue: [#69](https://github.com/Maxaubert/Wind/issues/69)
Spike spec: `docs/superpowers/specs/2026-05-29-dcomp-present-spike-design.md`
Status: design approved, pre-implementation

## Problem

The shipping overlay presents through a blt-model swapchain on a `WS_EX_LAYERED` window, which
microstutters through DWM (the layered window forbids flip-model). The spike (#69) measured the
three candidate present configs against a corrected cross-process click test and proved:

- `dcomp-nolayer` (no `WS_EX_LAYERED`) **eats cross-process clicks** -> dead.
- `dcomp-layered` (DirectComposition flip-model on a window that **keeps** `WS_EX_LAYERED`)
  **passes click-through, holds 144 fps, and adds no measured input latency** on a static desktop.

The one thing the static harness cannot reproduce is the background-game lag that got the prior
attempt (#24) reverted. So we add the `dcomp-layered` present path to the real engine behind an
opt-in toggle, leaving blt the untouched default, and the user decides in-game.

## Goal

Add a `present=dcomp` flip-model present path to `render_engine` as a hot-swappable A/B option
(default `present=blt`, unchanged), so the user can flip to DComp in a real game session, feel it,
and revert instantly. Delete blt only after dcomp is confirmed to win in-game (a later change, not
this one).

## Non-goals

- Not deleting or changing the default blt path. blt stays the default and stays fully intact.
- Not touching capture, magnify math, cursor mapping, lock detection, zoom, or multi-monitor logic.
  Only the present path (window content presentation) and the config plumbing change.
- Not the no-layer DComp config (proven to break clicks).
- Not resolving the game-lag question in code; that is the user's in-game A/B call.

## Toggle semantics (important)

`present` is NOT a per-frame hot-reload like `dwmFlush`/`vsync`. Switching it rebuilds the
swapchain (and, for dcomp, the DComp device/target/visual), which is unsafe mid-zoom-frame. It is
applied at a **zoom boundary**: when the ini changes `present`, the engine rebuilds the present
pipeline the next time the overlay is hidden (at 1x) or on the next zoom-in. No process restart;
the user flips `present=` in the ini, zooms out and back in, and is on the other path.

## Architecture

All changes are confined to the present path. The capture -> magnify -> cursor pipeline and its
constant buffer are unchanged except for the shader alpha output (see below).

### Components

**`src/config.h` / `src/config.cpp`** - add `std::string present = "blt";` parsed like the existing
string knob `cursorVisibility` (accepts `"blt"` | `"dcomp"`; any other value falls back to `"blt"`).
Serialize a `present=blt` default line with a comment in the config writer, next to `dwmFlush`.

**`src/render_engine.h`** - add `enum class PresentMode { Blt, Dcomp };`. Extend `initialize(...)`
with a `PresentMode present` argument (default `Blt`), and add `bool setPresentMode(PresentMode)`
to rebuild the present pipeline at runtime (returns false and keeps the current pipeline on
failure). Add `PresentMode presentMode() const` so `main` can detect whether a rebuild is needed.

**`src/render_engine.cpp`** - the bulk of the work:
- Add to `State`: `PresentMode present`, the flip swapchain (`ComPtr<IDXGISwapChain1> swapFlip`),
  and the DComp objects (`ComPtr<IDCompositionDevice> dcomp; IDCompositionTarget target;
  IDCompositionVisual visual;`). Keep the existing `swap` (blt) member.
- Factor the swapchain + RTV creation out of `initialize()` into `bool buildPresent()` (reads
  `s_->present`):
  - `Blt`: today's exact path (blt `DXGI_SWAP_EFFECT_DISCARD`, `BufferCount=1`,
    `SetMaximumFrameLatency(1)`, RTV from buffer 0). The window is created the same way it is now
    (`WS_EX_LAYERED | WS_EX_TRANSPARENT | ...`).
  - `Dcomp`: `CreateSwapChainForComposition` (FLIP_SEQUENTIAL, `BufferCount=2`,
    `DXGI_ALPHA_MODE_PREMULTIPLIED`, `SCALING_STRETCH`), then `DCompositionCreateDevice` ->
    `CreateTargetForHwnd(hwnd, TRUE)` -> `CreateVisual` -> `SetContent(swapFlip)` -> `SetRoot` ->
    `Commit`. The overlay window is unchanged and **keeps `WS_EX_LAYERED`** (the spike-proven
    click-through config; only the content source changes).
- `setPresentMode(mode)`: if `mode == s_->present` return true; else release the current present
  objects (rtv, swap/swapFlip, dcomp/target/visual), set `s_->present = mode`, call `buildPresent()`,
  and on success force a fresh capture (`freshCapture = true`, `haveDesktop = false`) so the next
  frame is live. On failure, restore the previous mode + rebuild it and return false.
- `renderFrame`: for `Dcomp`, re-acquire the RTV from `swapFlip` buffer 0 each frame (flip rotates
  buffers - established by the spike), present with `swapFlip->Present(1, 0)` (native flip pacing,
  no DwmFlush, ignores `p.vsync`). For `Blt`, the present path is exactly as today
  (`swap->Present(p.vsync ? 1 : 0, 0)`).
- `setVisible`:
  - `Blt`: today's `SetLayeredWindowAttributes` 0/255 toggle (unchanged).
  - `Dcomp`: per-pixel alpha. `setVisible(false)` clears the back buffer to transparent
    (0,0,0,0) and presents one frame (no magnify pass), so the overlay composes to nothing. The
    window stays shown the whole time (preserves the alt-tab no-flash invariant). `setVisible(true)`
    is a no-op for visibility (the next `renderFrame` presents opaque content, which is the reveal)
    but still re-asserts `HWND_TOPMOST` as the blt path does on show.
- `retarget`: the `ResizeBuffers` step runs on whichever swapchain is active (flip-model supports
  `ResizeBuffers`); rebind the RTV the same way. `invalidateCapture` is unchanged (it only resets
  the duplication, not the swapchain).

**`src/render_shaders.h`** - the magnify pixel shader outputs `alpha = 1.0` (opaque). Under blt
this is a no-op (visibility is governed by `LWA_ALPHA`); under dcomp premultiplied composition it
makes the shown frame fully opaque so it covers the desktop. The cursor pass keeps its alpha blend;
verify the cursor composites correctly under premultiplied output (the one spot that needs care -
#24 shipped this working, so the approach is known-good).

**`src/main.cpp`**:
- Pass `PresentModeFromCfg(cfg)` into `renderEngine.initialize(...)`.
- Pacing: when the engine's present mode is `Dcomp`, treat it like the vsync-present-paces branch -
  skip the timer wait and skip `DwmFlush()` (flip-model paces on `Present(1,0)`). Concretely:
  `dwmPaces` and the timer wait are gated to the blt path; dcomp always present-paces.
- Hot-reload: in `RunTick`'s config-reload block, if `nc.present != t.cfg.present`, remember it;
  apply it via `renderEngine.setPresentMode(...)` at the next zoom boundary - specifically in the
  zoom-in transition (where we already `retarget`/`invalidateCapture`) if not currently zoomed, or
  immediately if at 1x with the overlay hidden. Never rebuild mid-zoom-session.

### Data flow (unchanged except present)

`capture -> magnify (PS now emits alpha 1.0) -> cursor pass -> present`. For blt, present =
`Present(vsync,0)` (+ optional DwmFlush from the main loop). For dcomp, present =
`swapFlip->Present(1,0)` after re-acquiring the buffer-0 RTV; the DComp visual composites it.

## Invariants to preserve (verify each in implementation)

- `WDA_EXCLUDEFROMCAPTURE` on the overlay (no self-capture feedback loop). Window creation is
  unchanged, so this still applies in both modes.
- Cross-process click-through: dcomp keeps `WS_EX_LAYERED` (spike-proven PASS). Do not drop it.
- Alt-tab no-flash: window stays shown in both modes; dcomp hides via a transparent present, not
  SW_HIDE/SW_SHOW.
- Reveal-after-present ordering: blt presents-then-alpha; dcomp's opaque present *is* the reveal.
- Per-frame `HWND_TOPMOST` re-assert (displaced-only) - unchanged.
- HDR FP16 tonemap path - unchanged (it is in the magnify PS, which still runs; only the alpha
  output changes).
- Cursor restore on shutdown/crash - unchanged.

## Error handling

- `buildPresent()`/`setPresentMode()` failure: log via `RLog`, keep/restore the working mode, and
  return false so the caller stays on the previous (working) path. A dcomp build failure must never
  leave the engine without a present pipeline - fall back to rebuilding blt.
- `present` parse: unknown string -> `"blt"`.

## Testing / verification

- `build.bat test` stays green (pure-logic tests unaffected; `present` parse can get a unit test in
  the config test).
- `build.bat check` compiles all sources.
- `WIND_SELFTEST=1 Wind.exe` dumps a correct magnified PNG in BOTH modes (set `present=blt` and
  `present=dcomp`), including with HDR enabled (verify `wind_hdr_diag.txt`).
- `WIND_PACINGTEST=1 Wind.exe` runs the real present path for the configured mode and logs interval
  stats - run it for both modes for a CPU-side pacing comparison.
- Manual: zoom in/out in both modes; click-through to an app beneath while zoomed in dcomp; alt-tab
  then zoom (no flash); multi-monitor retarget in dcomp.
- The decider: user flips `present=dcomp` with a game running, A/Bs feel + in-game responsiveness.

## Out of scope / follow-ups

- Deleting the blt path (only after dcomp wins in-game).
- Any change to the no-layer config.
- Recording the in-game verdict in CLAUDE.md + closing/advancing #69.
