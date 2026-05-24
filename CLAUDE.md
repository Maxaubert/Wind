# Wind - fullscreen magnifier

Lightweight standalone Windows fullscreen magnifier replacing Magnify.exe.
Design spec: `docs/superpowers/specs/2026-05-24-magnifier-design.md`.
Plan: `docs/superpowers/plans/2026-05-24-wind-magnifier.md`.

## Commands
- Build app: `build.bat`  (locates MSVC via vswhere, emits `Wind.exe`)
- Build + run tests: `build.bat test`  (runs the doctest binary; exit 0 = pass)

## Stack
C++17, MSVC cl.exe. Windows Magnification API (`Magnification.lib`), Raw Input,
`WH_MOUSE_LL`, DWM (`Dwmapi.lib`). Tests: vendored `third_party/doctest.h`.

## Architecture
Pure logic (no `<windows.h>`): `src/transform`, `src/zoom_controller`, `src/tracker`,
parse half of `src/config`. Win32 I/O: `magnifier_engine`, `input_router`, `tray`,
`main`. A `DwmFlush`-paced tick thread reads shared atomics and calls
`MagSetFullscreenTransform(level, xOffset, yOffset)` each frame.

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

## Toolchain notes (this machine)
- VS 2026 Community is a prerelease channel, so `vswhere` needs `-all -prerelease`
  (NOT `-latest`) to find it. `build.bat` accounts for this.
- MSVC toolset 14.51.36231, Windows SDK 10.0.26100.0.

## Workflow
Feature/fix work: GitHub issue -> branch -> PR. README-only changes commit directly.
Current build runs local-only on branch `build-magnifier` (no remote yet).
