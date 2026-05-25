# Wind - Own Capture + GPU Renderer (Design Spec)

**Date:** 2026-05-25
**Status:** Approved for planning
**Branch:** to be created off the current work (e.g. `feat/own-renderer`)

## Goal

Replace the Windows Magnification API (for desktop use) with our own screen
capture + Direct3D renderer, so the magnified view pans with true sub-pixel
precision and the cursor is perfectly smooth at any zoom level - matching or
beating Windows Magnifier on the desktop. The existing Magnification-API engine
stays in the tree, selectable by config, so we can A/B the two and keep the
proven path for games.

## Background / why

The public Magnification API scales inside DWM and exposes only an **integer**
pixel offset (`MagSetFullscreenTransform(level, int xOffset, int yOffset)`). At
zoom L the view moves in L-screen-pixel steps, which reads as judder that gets
worse with zoom. There is no public sub-pixel path; Magnify uses a private
DWM/scanout route Microsoft does not expose. See `docs/PERFORMANCE-FINDINGS.md`.

The only third-party way to get sub-pixel control is to capture and render the
magnified image ourselves. On the desktop this is a clean win. It does **not**
help in games (an overlay over a borderless game forces composition anyway, the
same or slightly worse than today), which is why the Magnification-API engine is
kept for that case.

## Decisions (locked during brainstorming)

1. **Target:** perfect *desktop* magnifier. No injection, no per-game work, no
   anti-cheat risk.
2. **Engine strategy:** keep the existing Magnification-API engine untouched;
   build the new renderer on its own branch; select between them with an
   `engine=render|mag` config flag for an apples-to-apples comparison.
3. **Capture API:** Desktop Duplication (DXGI) + Direct3D 11. Decisive reason:
   it hands us the **cursor sprite separately** from the screen pixels, which is
   exactly what lets us magnify the content and stamp the real cursor at
   sub-pixel position. Pure C++/D3D (matches the stack), GPU texture with no
   copy, dirty-rect updates.
4. **Cursor feel:** **centered** - the cursor sits at screen center and the
   world pans smoothly under it; near desktop edges the cursor shifts off-center
   so corners are reachable. (Free/Magnify-style follow is explicitly out of
   scope for v1.)

## Architecture

### Data flow (per rendered frame)

1. `IDXGIOutputDuplication::AcquireNextFrame` -> desktop texture (cursor **not**
   baked in) + `DXGI_OUTDUPL_FRAME_INFO` (cursor position/visibility;
   `GetFramePointerShape` gives the cursor bitmap when it changes).
2. `cursor_mapper` (pure) integrates **raw-input deltas** into a float lens
   center in desktop space (sub-pixel), clamps it so the source rect stays on
   screen, and reports: the float source-rect top-left, the rendered cursor
   screen position (center, shifted near edges), and the desktop click point.
3. D3D draws the float source rect of the desktop texture scaled to fullscreen
   with bilinear sampling -> sub-pixel content pan, no judder.
4. D3D stamps the cached cursor sprite at the rendered cursor screen position
   (sub-pixel), scaled by zoom.
5. `SetCursorPos(round(clickPointDesktop))` keeps the OS cursor under the
   rendered cursor for click hit-testing (integer-desktop precision = sub-pixel
   at the magnified target, so clicks land dead-on).
6. Present via a flip-model swapchain on a fullscreen borderless topmost
   click-through window, vsync-paced to the display.

### Why the cursor is finally smooth

Today the magnified cursor is `integerDesktopPos x L`, so it hops L screen-pixels
per mouse-pixel. We break that by **decoupling the rendered cursor and lens from
the logical OS cursor**:

- The lens center (centered mode) is a **float** integrated from raw-input
  deltas - it moves in sub-desktop-pixel increments, so the content pans
  smoothly instead of in L-pixel steps.
- The rendered cursor is the **real sprite** (from Desktop Duplication, exact
  current shape) drawn at screen center via the sampler - it does not move, so
  there is nothing to judder.
- The OS cursor (integer desktop) is used only for clicks, synced via
  `SetCursorPos`.

This also clears both earlier fake-cursor failures: the mapping bug was a
coordinate-space error we now own end to end, and the game-cursor conflict is
moot because games stay on the Mag-API engine.

### Components / file structure (new branch)

- **New** `src/render_engine.{h,cpp}` - Desktop Duplication capture, D3D11
  device/swapchain, per-frame render + present, cursor sprite decode/cache,
  `DXGI_ERROR_ACCESS_LOST` recovery. Win32 / D3D I/O (not unit-tested).
- **New** `src/cursor_mapper.{h,cpp}` - **pure**, no `<windows.h>`: float
  raw-delta accumulation -> lens center, clamp to desktop bounds, rendered
  cursor screen position (centered + edge-shift), desktop click point.
  Unit-tested.
- **Extend** `src/transform` - float source-rect helper (sub-pixel
  `ComputeOffset` returning doubles) alongside the existing integer one.
- **Extend** `src/config` - `engine` (`render`|`mag`), cursor sensitivity,
  cursor scale-with-zoom toggle, scaling filter (`bilinear` default).
- **Modify** `src/main.cpp` - select engine by config; wire `render_engine` +
  `cursor_mapper`; feed raw-input deltas into `cursor_mapper`.
- **Delete** `src/cursor_overlay.{h,cpp}` - failed fake-cursor experiment.
- **Untouched** `magnifier_engine`, `input_router`, `tray`, `zoom_controller`,
  `tracker`.

### Window & input model

- Overlay window: `WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST |
  WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW`, fullscreen, click-through. It is purely
  a display surface; clicks pass through to the real apps at the synced OS-cursor
  position (so no input remapping / UIAccess is needed for the render engine).
- Cursor hide: the OS cursor must be hidden under the overlay or we get a double
  cursor. **This is the #1 risk and gets a spike before anything else is built.**
  Candidate mechanisms, in order: `MagShowSystemCursor(FALSE)` (lightweight; may
  work without an active transform), then `SetSystemCursor` with a blank cursor
  (global; must restore on **every** exit path, including crash), then a
  capture-based hide.

## Performance & pacing

- `AcquireNextFrame` wakes on content change or cursor move; we also re-render
  when we pan or zoom even if the desktop is static. Idle is cheap (DDA timeout
  lets the loop sleep, no re-render).
- Flip-model swapchain (`DXGI_SWAP_EFFECT_FLIP_DISCARD`), vsync present at the
  display refresh (144 Hz here).
- Cost vs the in-compositor Mag API: ~1 extra frame of latency (~7 ms at 144 Hz)
  from capture -> render -> present. Acceptable for desktop; this is precisely
  why the Mag-API engine is kept for games.

## Scope boundaries (v1)

- **Single monitor** (primary output). Multi-monitor is future work.
- **DRM-protected video** (e.g. Netflix, some players) renders **black in the
  magnified layer** - a hard Desktop Duplication limitation. The normal desktop
  is unaffected; only the zoomed overlay shows black over protected surfaces.
- **SDR assumed.** HDR desktop capture (FP16 swapchain) is future.
- Per-Monitor-V2 DPI awareness retained (physical-pixel math, as today).
- No game auto-detection; engine is chosen by the `engine` config flag.

## Testing & verification

- **Pure logic** (`cursor_mapper`, float `transform`, `tracker`, `config`) ->
  doctest, desktop-free, compiled with `WIND_TESTS` (same harness as today).
  `cursor_mapper` is the bug-prone math and gets thorough tests: sub-pixel lens
  accumulation, edge clamping, rendered-cursor edge-shift, and desktop
  click-point back-computation.
- **D3D/DDA/window** (not unit-testable) -> manual visual verification, a
  PresentMon pacing capture, and a side-by-side feel test vs Windows Magnifier
  (the comparison this branch exists for).
- The pre-build **cursor-hide spike** must pass (single cursor visible, restored
  cleanly on exit) before the renderer is built out.

## Risks

1. **OS-cursor hide** under a click-through overlay (spike first - see above).
2. **Cursor/click coordinate mapping** across Per-Monitor-V2 DPI (the earlier
   fake-cursor bug). Mitigated by owning the transform end to end and
   unit-testing the math in `cursor_mapper`.
3. **`DXGI_ERROR_ACCESS_LOST`** on resolution change, secure desktop (UAC), and
   fullscreen transitions - must recreate the duplication gracefully.
4. **DRM black** surfaces (documented limitation, accepted for v1).

## Out of scope (v1)

Free/Magnify-style follow cursor; multi-monitor; HDR; game auto-detection;
in-game rendering (injection). All are possible follow-ups but excluded here.
