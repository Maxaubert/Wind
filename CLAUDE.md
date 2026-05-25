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
C++17, MSVC cl.exe. DXGI Desktop Duplication + Direct3D 11 (own renderer); Windows
Magnification API (`Magnification.lib`); Raw Input, `WH_MOUSE_LL`, DWM (`Dwmapi.lib`),
WIC. Tests: vendored `third_party/doctest.h`.

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
- RENDER ENGINE: cross-process click-through needs `WS_EX_LAYERED | WS_EX_TRANSPARENT`
  (+ `SetLayeredWindowAttributes(.,255,LWA_ALPHA)`). `WS_EX_TRANSPARENT` + HTTRANSPARENT
  alone only forwards to *same-thread* windows, so clicks to other apps get eaten. Layered
  rules out a flip swapchain, so the overlay uses a BLT-model swapchain (DXGI_SWAP_EFFECT_
  DISCARD) - verified to display via the redirection surface. Latency capped with
  `IDXGIDevice1::SetMaximumFrameLatency(1)`.
- RENDER ENGINE: never leave the OS cursor hidden. `shutdown()` restores via
  `MagShowSystemCursor(TRUE)` + `MagUninitialize` + `SystemParametersInfo(SPI_SETCURSORS)`,
  plus a `SetUnhandledExceptionFilter` net for crashes.
- Verify the render overlay only from INSIDE the app (it is capture-excluded, so external
  screenshots can't see it): `WIND_SELFTEST=1 Wind.exe` dumps `wind_selftest.png`.

## Toolchain notes (this machine)
- VS 2026 Community is a prerelease channel, so `vswhere` needs `-all -prerelease`
  (NOT `-latest`) to find it. `build.bat` accounts for this.
- MSVC toolset 14.51.36231, Windows SDK 10.0.26100.0.

## Workflow
Feature/fix work: GitHub issue -> branch -> PR. README-only changes commit directly.
Remote: `github.com/Maxaubert/Wind`. Own-renderer work is on `feat/own-renderer` (issue #4).
