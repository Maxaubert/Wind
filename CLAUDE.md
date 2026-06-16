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
- Build config UI: `build.bat config`  (npm-builds the Svelte app under `ui/` to `ui/dist/`, then
  compiles `src/config_ui/*.cpp` against the vendored WebView2 SDK -> `WindConfig.exe` next to
  `Wind.exe`). Also run by `tools\uiaccess_setup.ps1`, which deploys `WindConfig.exe` + `ui/dist`
  alongside the signed `Wind.exe`.
- Deploy UIAccess build (elevated; from a normal shell):
  `Start-Process powershell -Verb RunAs -ArgumentList '-NoExit','-ExecutionPolicy','Bypass','-File','tools\uiaccess_setup.ps1'`

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

**Two binaries.** `Wind.exe` is the always-running tray magnifier (the perf-critical core
described above). `WindConfig.exe` is an on-demand settings GUI: a thin C++ WebView2 host
(`src/config_ui/main.cpp`) that loads a built Svelte app from `ui/dist/` and talks to the core
only by writing `magnifier.ini` (the core dir-watches and hot-reloads it - no IPC). First
launch also runs a short guided onboarding (wind-trails-into-logo intro -> set zoom keys ->
done; sets `onboarded=1` so it never auto-opens again). The config process is non-admin, runs
in a separate exe entirely, and has zero perf coupling to the magnifier loop. Settings spec:
`docs/superpowers/specs/2026-05-27-config-ui-polish-onboarding-design.md`. UI source: `ui/src/`
(Svelte + Vite). Bridge messages: `getConfig`, `setConfig`, `window` (minimize/close),
`openIni`. Settings live-applies keybind changes (sync `values`+`saved`); other rows use the
staged Apply/Discard footer.

## IMPORTANT gotchas
- Pure-logic files MUST NOT include `<windows.h>` - keeps unit tests desktop-free.
  The test build compiles only the pure `.cpp` files and defines `WIND_TESTS`.
- INPUT SWALLOWING: bound keybinds are eaten so they never double-fire into the focused app. Mouse
  side-buttons go through the `WH_MOUSE_LL` hook; keyboard zoom/recenter binds go through a
  `WH_KEYBOARD_LL` hook (both on the same dedicated hook thread, `input_router.cpp`). A swallowed key
  never appears in `GetAsyncKeyState`, so the keyboard hook is the AUTHORITY for bound-key down-state
  (`keyPressed()`); `main.cpp` reads it when `kbHookActive()`, else falls back to polling (install
  failure / `WIND_NOHOOK`). hide-cursor + hotkey-mode quick-zoom are swallowed by `RegisterHotKey`
  instead, not this hook. SAFETY: `IsForbiddenBindVk` (pure, in `config.cpp`) blocks binding keys
  that would be catastrophic to lose system-wide - left/right click (1/2), Backspace (8), Win
  (0x5B/0x5C) - enforced in three places: the hook never swallows them, `ParseConfig` sanitizes them
  out of the ini, and the config UI's keybind capture refuses them. Down/up swallows are balanced
  (only swallow an UP whose DOWN we swallowed) and released on teardown so a key is never stranded.
  LIMITATION (by design, not fixable in user mode): LL hooks swallow only the legacy/cooked input
  path (`WM_*`, `GetAsyncKeyState`) that desktop apps and browsers use. They CANNOT block Raw Input
  (`WM_INPUT`), which most GAMES read directly - so a bound key/button still reaches a raw-input game
  no matter what the hook returns. There is no user-mode API to suppress raw input to another
  process; the only reliable fix is a kernel filter driver (e.g. Interception), which we deliberately
  do NOT use (no-driver design + anti-cheat ban risk). Confirmed: swallowing works in normal apps,
  not in raw-input games. Pick game keys/buttons you don't otherwise use.
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
  alone only forwards to *same-thread* windows, so clicks to other apps get eaten. The layered
  window's swapchain is blt-model (`DXGI_SWAP_EFFECT_DISCARD`), composited through the DWM
  redirection surface. Latency capped with `IDXGIDevice1::SetMaximumFrameLatency(1)`.
- RENDER ENGINE: present pacing is the blt-model swapchain through DWM. It NEVER tears (DWM always
  composites it at vblank). Its one artifact is a phase-mismatch microstutter (NOT our loop - proven
  clean at 144fps via `WIND_PACINGTEST`), tamed by the `dwmFlush` knob: `dwmFlush=0` (default) =
  plain vsync `Present(1,0)`; `dwmFlush=1` = present immediately (`Present(0,0)`) then `DwmFlush()`
  to align 1:1 with composition. Both hot-reloadable.
- RENDER ENGINE: DO NOT re-attempt a DirectComposition flip-model present path. It was tried and
  abandoned TWICE (#11, #69): a flip-model swapchain on the layered HWND (via an `IDCompositionVisual`)
  presents, but DWM promotes the fullscreen visual to an independent-flip / MPO plane that scans out
  unsynced and TEARS on any frame hitch - badly on a VRR/G-SYNC display (confirmed via diagnostics:
  tear correlates 1:1 with loop hitches at a steady physical refresh). Forcing it onto the composited
  path with `dwmFlush=1` stops the tear but then chains us to the VRR-floated composite rate (drooped
  to ~68Hz on a 23-143Hz panel). A "composition pin" (forever-animating child visual) was also tried
  and made tearing worse. Net: dcomp is never a win over blt on this layered click-through overlay.
  RTSS overlay is a quick tell - it shows over blt (hookable composited path), vanishes over dcomp.
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
- MULTI-MONITOR: `multiMonitor=1` magnifies the monitor the cursor is on at each zoom-in; `0`
  (the shipped default) = primary only. The overlay is moved/resized and the DXGI output is re-selected
  by device name (`render_engine` `retarget`/`selectOutput`); the pipeline works in LOCAL monitor
  pixels with a `(originX,originY)` offset applied only at `GetCursorPos`/`SetCursorPos`. Limit:
  if the cursor's monitor is on a DIFFERENT GPU than our D3D device, `retarget` returns false and
  we keep the current monitor (no cross-adapter chase). While zoomed you stay on one monitor
  (the OS cursor is pinned to it); switch by zooming out and back in on the other one.
- CURSOR SENSITIVITY auto-matches the real OS cursor: while zoomed (cursor hidden), each tick reads
  the OS cursor's own movement since our last `SetCursorPos` (Windows' pointer acceleration already
  applied) and pans by that scaled by `cursorSensitivity` (default 1.0 = exact match), so panning
  equals the user's normal cursor without reimplementing ballistics, with an optional speed multiplier
  on top. `GetCursorPos` works as this "oracle" only because we read it BEFORE re-setting it each
  tick. Raw mickeys are kept solely to (a) feed `LockDetector` (a game clipping/recentering the cursor
  -> `GetClipCursor` confined, or raw-active-but-cursor-frozen with hysteresis) and (b) drive panning
  while locked (also scaled by `cursorSensitivity`). Both regimes integrate a DELTA into the same
  accumulator, so a free/locked switch never snaps position (avoids the old Tracker flicker, issue #3).
  The click point, drawn cursor, and view all derive from the SMOOTHED center (`cx_`), so a click lands
  under the visible cursor; do not "fix" the click/warp point to the unsmoothed target (it would
  misalign clicks) and do not revert to a fixed sensitivity multiplier.
- PROGRAM FILES IS READ-ONLY FOR NON-ADMIN: any file the runtime needs to write MUST go to a
  per-user-writable location, never next to the exe. The UIAccess build is installed to
  `C:\Program Files\Wind\` and WindConfig.exe runs as a normal user, so an in-place write there
  silently fails (Apply / live keybind capture / WebView2 init all break this way historically).
    - magnifier.ini: ALWAYS resolve the path via `wind::ResolveIniPath()` (src/config_path.h),
      used by both Wind.exe core and WindConfig.exe host. It probes whether the exe dir is
      writable; dev keeps the ini next to the exe, Program Files transparently falls back to
      `%LOCALAPPDATA%\Wind\magnifier.ini` and seeds it from the install template on first launch
      so deploy-time defaults carry over. Never hardcode `L"magnifier.ini"` (it would re-break
      the Program Files deploy on the next feature that touches the ini).
    - WebView2 user-data folder: WindConfig.exe explicitly passes `%LOCALAPPDATA%\Wind\WebView2`
      to `CreateCoreWebView2EnvironmentWithOptions`. The default (`<exeDir>\WindConfig.exe.WebView2`)
      is read-only in Program Files, which makes the env creation fail and the window paint as
      an empty shell. Keep the explicit path when touching the host's env setup.
    - Diagnostics: the unified logger writes rolling per-process logs (`wind-core.log` /
      `wind-config.log`), the startup system snapshot, and crash dumps (`wind-crash-*.dmp/.txt`) to
      `%LOCALAPPDATA%\Wind\logs\` (resolved via `wind::ResolveLogDir`; src/logging.*). The tray and
      WindConfig "Export diagnostics" action zips that folder to the Desktop (Compress-Archive). The
      opt-in `diagnostics=1` frame-pacing trace still goes to `%TEMP%\wind_diag.log` separately;
      `wind_selftest.png` is dev-only (env-gated).

## Toolchain notes (this machine)
- VS 2026 Community is a prerelease channel, so `vswhere` needs `-all -prerelease`
  (NOT `-latest`) to find it. `build.bat` accounts for this.
- MSVC toolset 14.51.36231, Windows SDK 10.0.26100.0.

## Workflow
Feature/fix work: GitHub issue -> branch -> PR. README-only changes commit directly.
Remote: `github.com/Maxaubert/Wind`. Own-renderer work is on `feat/own-renderer` (issue #4).

## Style
- NEVER use em-dashes (the "—" character) anywhere: code, comments, docs, commit messages,
  and UI copy. Use en-dashes, commas, or rephrase. Avoid the `&mdash;` HTML entity in UI strings too.
