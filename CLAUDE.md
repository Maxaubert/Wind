# Wind - fullscreen magnifier

Lightweight standalone Windows fullscreen magnifier replacing Magnify.exe.
Design spec: `docs/superpowers/specs/2026-05-24-magnifier-design.md`.
Plan: `docs/superpowers/plans/2026-05-24-wind-magnifier.md`.

## Commands
- Build app: `build.bat`  (locates MSVC via vswhere, emits `Wind.exe`; uiAccess=false, runs anywhere)
- Build + run tests: `build.bat test`  (runs the doctest binary; exit 0 = pass)
- Build UIAccess variant: `build.bat uiaccess`  (uiAccess=true manifest; must be signed + run
  from `C:\Program Files\Wind` - deploy via `tools\uiaccess_setup.ps1` elevated). Needed only
  to cover the Start menu / taskbar / tray (overlay uses `CreateWindowInBand`, `zorderBand=16`).

## Stack
C++17, MSVC cl.exe. DXGI Desktop Duplication + Direct3D 11 (own renderer) presented via
DirectComposition + flip-model swapchain (`Dcomp.lib`); Windows Magnification API
(`Magnification.lib`); Raw Input, `WH_MOUSE_LL`, DWM (`Dwmapi.lib`), WIC. Tests: vendored
`third_party/doctest.h`.

## Architecture
Pure logic (no `<windows.h>`): `src/transform` (+ float `ComputeOffsetF`),
`src/zoom_controller`, `src/tracker`, `src/cursor_mapper`, parse half of `src/config`.
Win32 I/O: `magnifier_engine`, `render_engine`, `input_router`, `tray`, `main`.

Two selectable engines (`engine=render|mag` in magnifier.ini), one paced tick loop:
- `render` (default): `render_engine` - own DXGI Desktop Duplication capture + D3D11.
  Magnifies a sub-pixel float source rect to a click-through, capture-excluded
  (`WDA_EXCLUDEFROMCAPTURE`) fullscreen overlay; draws the real cursor (`GetCursorInfo`)
  centered via `cursor_mapper`; hides the OS cursor (`MagShowSystemCursor`) and syncs
  `SetCursorPos` for clicks. Sub-pixel pan + smooth centered cursor. No UIAccess needed.
- `mag`: `magnifier_engine` - Windows Magnification API (`MagSetFullscreenTransform`),
  kept for games (integer offset, rides DWM).
Spec: `docs/superpowers/specs/2026-05-25-own-renderer-design.md`. Issue #4.

## IMPORTANT gotchas
- Pure-logic files MUST NOT include `<windows.h>` - keeps unit tests desktop-free.
  The test build compiles only the pure `.cpp` files and defines `WIND_TESTS`.
- Declare Per-Monitor-V2 DPI awareness (`Wind.manifest`) or offset pixel math is wrong
  on scaled displays.
- Always reset to `MagSetFullscreenTransform(1.0,0,0)` + `MagUninitialize` on exit -
  never leave the screen zoomed.
- The lens-must-move-when-cursor-locked behavior is THE core feature. It relies on
  Raw Input deltas (HID-level, unaffected by ShowCursor/ClipCursor/SetCursorPos),
  NOT GetCursorPos, when a lock is detected. Do not "simplify" this away.
- `MagSetInputTransform` is intentionally NOT used (needs UIAccess). Visual-only.
- RENDER ENGINE: the overlay MUST set `SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE)` or
  Desktop Duplication captures our own presented frame -> we magnify our own output ->
  feedback loop (black). This is the #1 render-engine gotcha.
- RENDER ENGINE: the overlay uses **DirectComposition + a flip-model swapchain**
  (`CreateSwapChainForComposition`, `DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL`, `ALPHA_MODE_PREMULTIPLIED`)
  on a `WS_EX_NOREDIRECTIONBITMAP | WS_EX_TRANSPARENT` window (NOT `WS_EX_LAYERED`). Flip-model
  is what gives smooth pacing - the old blt-model + layered swapchain microstuttered constantly
  (our loop was proven clean at 144fps; the judder was DWM compositing blt-model). Per-pixel
  alpha replaces `LWA_ALPHA`. Cross-process click-through still works via `WS_EX_TRANSPARENT`
  (verified in `tools/dcomp_clickthrough_test.cpp`: WindowFromPoint returns the window beneath).
  The magnify PS must output **alpha 1.0** (opaque) or premultiplied composition makes the
  shown overlay see-through. Latency capped with `IDXGIDevice1::SetMaximumFrameLatency(1)`.
- RENDER ENGINE: never leave the OS cursor hidden. `shutdown()` restores via
  `MagShowSystemCursor(TRUE)` + `MagUninitialize` + `SystemParametersInfo(SPI_SETCURSORS)`,
  plus a `SetUnhandledExceptionFilter` net for crashes.
- RENDER ENGINE: show/hide by drawing transparent vs opaque, NOT `SW_HIDE`/`SW_SHOW`. The window
  is always shown; "hide" (`setVisible(false)`) presents one fully transparent frame (clear to
  `{0,0,0,0}`) so the overlay goes see-through; "show" is implicit (the next `renderFrame` draws
  the opaque zoomed view). With per-pixel alpha + no hide/show there's no DWM frame caching, which
  fixes the alt-tab zoom-in flash (#8). (Pre-DComp this was an `SW_HIDE`->`SW_SHOW` cache bug.)
- RENDER ENGINE: stay above EVERYTHING - re-assert `HWND_TOPMOST` every frame in `renderFrame`
  (transparent + click-through + capture-excluded, so being on top is safe). If we sit below an
  always-on-top app overlay (RTSS, Task Manager), that window draws a second unmagnified copy
  over our magnified view. `zorderBand=16` (signed UIAccess build) also covers shell + same-band.
- RENDER ENGINE: on zoom-in, `invalidateCapture()` + `capture()` drains to the LATEST duplication
  frame (not the first): the first AcquireNextFrame after (re)creating the duplication can be a
  transitional composite (the window underneath), which otherwise flashed on reveal.
- Verify the render overlay only from INSIDE the app (it is capture-excluded, so external
  screenshots can't see it): `WIND_SELFTEST=1 Wind.exe` dumps `wind_selftest.png`.

## Toolchain notes (this machine)
- VS 2026 Community is a prerelease channel, so `vswhere` needs `-all -prerelease`
  (NOT `-latest`) to find it. `build.bat` accounts for this.
- MSVC toolset 14.51.36231, Windows SDK 10.0.26100.0.

## Workflow
Feature/fix work: GitHub issue -> branch -> PR. README-only changes commit directly.
Remote: `github.com/Maxaubert/Wind`. Own-renderer work is on `feat/own-renderer` (issue #4).
