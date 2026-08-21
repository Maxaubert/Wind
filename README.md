<div align="center">
  <img src="assets/wind-badge.svg" alt="Wind" width="128">

  # Wind

  Barely there. Everywhere.

  A lightweight magnifier for Windows.

  [![Windows](https://img.shields.io/badge/Windows-10%20%7C%2011-0078D4?style=flat-square)](https://github.com/Maxaubert/Wind)
  [![Built with](https://img.shields.io/badge/C%2B%2B-Direct3D%2011-00599C?style=flat-square)](https://github.com/Maxaubert/Wind)

  [Download](https://github.com/Maxaubert/Wind/releases) · [Documentation](docs/architecture/README.md)
</div>

---

https://github.com/user-attachments/assets/cf86509c-e5cd-4055-bfce-ca385de72965

A replacement for the built-in Magnifier, with smooth continuous zoom that keeps tracking the
mouse even when games hide, clip, or center-lock the cursor.

> Contributing or curious how it works? The developer book lives at
> [docs/architecture](docs/architecture/README.md): twelve chapters covering every subsystem,
> end to end.

Wind renders the magnified view itself - capturing the desktop with DXGI Desktop Duplication
and scaling it on the GPU (Direct3D 11) onto a click-through overlay, or magnifying inside the
compositor (DWM fullscreen transform) when a game is in front. That gives sub-pixel smooth
panning and a crisp cursor that the integer-offset Windows Magnification API can't, and lets
you keep clicking and using the screen while zoomed.

## Features
- **Smooth, sub-pixel zoom and pan** with light inertia - no stepping or cursor hop.
- **Interact while zoomed** - clicks pass through to the app under the cursor.
- **Auto engine per situation** - games get the compositor-internal transform path (stays
  smooth under heavy GPU load), everything else the high-fidelity render overlay; Wind switches
  live when you alt-tab, keeping the zoom level.
- **Named settings profiles** - full snapshots (keybinds included), switchable from the tray
  menu or the Settings titlebar.
- **Real cursor** - the actual pointer shapes (text I-beam and link hand included), drawn
  crisp at every zoom.
- **HDR-aware** - on an HDR display it tonemaps to match the desktop automatically (tracking
  the live SDR-brightness slider); on SDR it's a straight passthrough. No per-machine tuning.
- **Follows the mouse even when a game locks/hides the cursor** (HID-level Raw Input, no
  injection - anti-cheat safe).
- **Inspect mode** - freeze the cursor (keeps a hover/tooltip alive) and free-look around with
  a crosshair; clicks land where you aim.
- **Zoom lock detection** - games that pin the mouse to the screen center (DOOM-style
  mouselook) would drag the zoom back with it; listed apps (Settings > Cursor) pan from raw
  mouse motion instead.

## Magnifier models (`model=`)
Selected with the `model` ini key or the "Magnifier engine" row in Settings. `model` is
read once at launch, so switching it restarts Wind (Settings does this automatically on Apply).

- **`hybrid`** (default, shown as **Auto**) - constructs both engines below and picks per
  zoom-in: the transform for a borderless-fullscreen foreground on the primary monitor (games,
  F11 video), the render overlay for everything else. Re-picks live when the foreground
  changes mid-zoom.
- **`render`** - captures the desktop with DXGI Desktop Duplication and redraws it into a
  D3D11 overlay. The cursor is drawn into the same frame as the content, so it can never drift
  against the view. The only model that can cover the shell (see the `zorderBand` note below).
- **`transform`** - the DWM fullscreen transform only (what `hybrid` uses over games), with no
  overlay at all. Compositor-internal, so it stays smooth while a heavy game renders.
- **`magnify`** - drives the **native Windows Magnifier** with Wind's buttons. This is the
  model for DRM-protected video (Netflix and friends), which shows black under screen capture.
  Holding a zoom button scroll-zooms Magnifier exactly like its own Ctrl+Alt+wheel shortcut,
  stepping by `magnifyStep` percent per notch (ini key; lower = smoother and slower, applies
  live). Everything else is pure native Magnifier behavior; quitting Wind
  (or switching models) closes it and restores your original Magnifier settings. Max zoom is
  Magnifier's ceiling, 1600%.

The `magnify` model hands the view and cursor to Windows Magnifier, so the render-only features
do not apply there: `sharpness`, `hdrTonemap`, `bilinear`, `outline*`, `brightness`,
`cursorSensitivity`/`cursorSmoothing`, `multiMonitor`, and Inspect mode.

## Controls
Zoom binds ship **unbound** - the first-launch guided setup captures your choice (mouse
side-buttons and/or keyboard keys, with optional alternates). Everything is rebindable in
Settings; bound keys are swallowed so they never double-fire into the focused app.

- Hold your **zoom-in** bind - zoom in (smooth ramp). Hold **zoom-out** - zoom back.
- Release - zoom stays at the current level.
- **Quick zoom** (default Ctrl + a zoom key, or a dedicated hotkey) - toggle between 1x and
  your remembered level.
- **Inspect mode** (optional bind) - freeze the cursor and free-look with the crosshair.
- **Ctrl+Alt+Q** - quit from anywhere (also restores the cursor); or use the tray icon.

## Releases
Download `Wind-Setup-x64-<version>.exe` from the
[Releases page](https://github.com/Maxaubert/Wind/releases) and run it.

Setup installs **per-machine** to `C:\Program Files\Wind` and asks for administrator rights.
That location is not a preference: Windows only grants UIAccess to a signed binary in a
"secure location", and UIAccess is what lets Wind's shortcuts keep working while an elevated
window has focus, and what enables the desktop zoom path. Setup also offers to start Wind when
you sign in, and installs the WebView2 runtime if Settings has no browser engine to run in.
Your settings, profiles and logs stay in `%LOCALAPPDATA%\Wind`, and uninstalling keeps them
unless you say otherwise.

**Signing.** Release builds are currently **unsigned**, so Windows SmartScreen will warn on
first run, and the UIAccess-only behaviour above is switched off (Wind detects this at startup
and stays on the render path for the desktop; everything else works normally). A free
open-source certificate is being sought from
[SignPath Foundation](https://signpath.org/); the release pipeline already signs when one is
configured, via `WIND_SIGN_THUMBPRINT`, or `WIND_SIGN_PFX` plus `WIND_SIGN_PASSWORD`:

```
pwsh -File tools\release.ps1
```

With a certificate it builds the UIAccess variant, signs both executables and the installer,
and writes `dist\Wind-Setup-x64-<version>.exe`. Without one it builds the ordinary variant and
says so. `src\version.h` is the only place the version is declared.

## Build
Requires Visual Studio 2022+ Build Tools (Desktop development with C++). From any shell:
- `build.bat` - builds `Wind.exe` (runs from anywhere).
- `build.bat test` - builds and runs the unit tests.
- `build.bat uiaccess` - builds the UIAccess variant (signed-install prerequisite).
- `build.bat config` - builds the Settings app (`WindConfig.exe` + the Svelte UI).
- `build.bat installer` - compiles the setup program (needs NSIS: `winget install NSIS.NSIS`).

## Install from source (development)
Run **elevated**:
```
powershell -ExecutionPolicy Bypass -File tools\uiaccess_setup.ps1
```
This builds the UIAccess variant and the Settings app, self-signs them, and installs to
`C:\Program Files\Wind`. Launch `C:\Program Files\Wind\Wind.exe` from a normal (non-elevated)
window so UIAccess engages. Settings (and the ini) live per-user under `%LOCALAPPDATA%\Wind`.

Note on shell coverage: covering the Start menu / taskbar / tray flyouts additionally requires
the opt-in `zorderBand=16` (UIAccess build only). It ships **off** (`zorderBand=0`) because the
high band puts Wind under the Snipping Tool's capture overlay, which costs the cursor entirely
during Win+Shift+S - a deliberate trade-off (issue #162).

## Config (`magnifier.ini`, hot-reloads unless noted)
The Settings app (tray -> Open Settings) is the comfortable way to edit this file; it keeps
the everyday settings front and center (the rest sit behind "Show advanced settings") and
closes itself if the magnifier exits. Every ini key below keeps working even when it has no
Settings row.
Profiles (tray -> Profiles, or the Settings titlebar) snapshot the whole file per activity.

- `zoomInButton`/`zoomOutButton` (mouse side-buttons) and `zoomInVk`/`zoomOutVk` (keyboard) -
  hold to zoom; all ship unbound until the guided setup. Alternates: `*2` variants.
- `maxLevel`, `zoomInSpeed`/`zoomOutSpeed`, `smoothZoom*` - zoom range and feel.
- `cursorSensitivity`, `cursorSmoothing` - pan speed and inertia.
- `bilinear`, `sharpness`, `cursorScaleWithZoom` (default 0: constant cursor size),
  `cursorVisibility` - image and cursor rendering.
- `brightness`, `hdrTonemap` - output tuning.
- Pacing/perf: `vsync` (default on), `dwmFlush` (default 0), `gameFpsCap`, `gpuPriority`.
- `model` - `hybrid` (default) / `render` / `transform` / `magnify`. Restart to switch.
- `multiMonitor` - 0 (default, primary only) or 1 (follow the cursor's monitor per zoom-in).
- `desktopTransform` - experimental, ini-only: use the game (compositor) engine on the
  desktop too (signed install only, primary monitor only, Auto model).
- `lockApps` - per-app zoom lock detection (Settings > Cursor > "Zoom lock detection");
  `warpLock=1` extends the detection heuristics to unlisted games.
- Advanced: `zorderBand`, `transformExclude`, `noSwallowApps`, `profile`.

## Scope
Primary monitor by default (`multiMonitor=1` follows the cursor's monitor). Covers the desktop,
normal apps, and **borderless / windowed-fullscreen** games. Exclusive-fullscreen games are out
of scope (set the game to borderless).
