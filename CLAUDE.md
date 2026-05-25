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
C++17, MSVC cl.exe. DXGI Desktop Duplication + Direct3D 11 (own renderer); Raw Input,
`WH_MOUSE_LL`, DWM (`Dwmapi.lib`), WIC, `MagShowSystemCursor` (`Magnification.lib`, just to
hide the OS cursor). Tests: vendored `third_party/doctest.h`.

## Architecture
Pure logic (no `<windows.h>`): `src/transform` (float `ComputeOffsetF`),
`src/zoom_controller`, `src/cursor_mapper`, parse half of `src/config`.
Win32 I/O: `render_engine`, `input_router`, `tray`, `main`.

One engine, one paced tick loop. `render_engine` = own DXGI Desktop Duplication capture +
D3D11: magnifies a sub-pixel float source rect to a click-through, capture-excluded
(`WDA_EXCLUDEFROMCAPTURE`) fullscreen overlay; draws the real cursor (`GetCursorInfo`) centered
via `cursor_mapper`; hides the OS cursor (`MagShowSystemCursor`) and syncs `SetCursorPos` for
clicks. Sub-pixel pan + smooth centered cursor. The old Magnification-API `engine=mag` fallback
was removed (issue #20) - render is the only engine.
Spec: `docs/superpowers/specs/2026-05-25-own-renderer-design.md`. Issue #4.

## IMPORTANT gotchas
- Pure-logic files MUST NOT include `<windows.h>` - keeps unit tests desktop-free.
  The test build compiles only the pure `.cpp` files and defines `WIND_TESTS`.
- Declare Per-Monitor-V2 DPI awareness (`Wind.manifest`) or offset pixel math is wrong
  on scaled displays.
- The lens-must-move-when-cursor-locked behavior is THE core feature. It relies on
  Raw Input deltas (HID-level, unaffected by ShowCursor/ClipCursor/SetCursorPos),
  NOT GetCursorPos, when a lock is detected. Do not "simplify" this away.
- Clicks are routed by syncing `SetCursorPos` under the drawn cursor (NOT `MagSetInputTransform`,
  which needed UIAccess and is no longer used anywhere).
- RENDER ENGINE: the overlay MUST set `SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE)` or
  Desktop Duplication captures our own presented frame -> we magnify our own output ->
  feedback loop (black). This is the #1 render-engine gotcha.
- RENDER ENGINE: cross-process click-through needs `WS_EX_LAYERED | WS_EX_TRANSPARENT`
  (+ `SetLayeredWindowAttributes(.,255,LWA_ALPHA)`). `WS_EX_TRANSPARENT` + HTTRANSPARENT
  alone only forwards to *same-thread* windows, so clicks to other apps get eaten. Layered
  rules out a flip swapchain, so the overlay uses a BLT-model swapchain (DXGI_SWAP_EFFECT_
  DISCARD) - verified to display via the redirection surface. Latency capped with
  `IDXGIDevice1::SetMaximumFrameLatency(1)`.
- RENDER ENGINE: blt-model present through DWM microstutters - a phase mismatch with DWM's
  compositor, NOT our loop (proven clean at 144fps via `WIND_PACINGTEST`). Default pacing is
  `dwmFlush=1`: present immediately (`Present(0,0)`) then `DwmFlush()` to block until DWM's next
  composite, aligning our frames 1:1 with composition. `dwmFlush=0` = old vsync `Present(1,0)`;
  both hot-reloadable. DirectComposition + flip-model would be smoother but requires dropping
  `WS_EX_LAYERED`, which breaks cross-process click-through (tried in #11, reverted) - so it's a
  dead end unless a non-layered click-through is found. Issue #9.
- RENDER ENGINE: never leave the OS cursor hidden. `shutdown()` restores via
  `MagShowSystemCursor(TRUE)` + `MagUninitialize` + `SystemParametersInfo(SPI_SETCURSORS)`,
  plus a `SetUnhandledExceptionFilter` net for crashes.
- RENDER ENGINE: show/hide the overlay by toggling the layer alpha (`SetLayeredWindowAttributes`
  0/255), NOT `SW_HIDE`/`SW_SHOW`. A layered window that is hidden then re-shown makes DWM cache
  and re-display the frame from when it was last visible, flashing the previous zoom session's
  window on the next zoom-in (worst right after an alt-tab). The window is created shown at
  alpha 0 and stays shown. On zoom-in, present the live frame FIRST, then flip alpha to 255.
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
