<div align="center">
  <img src="assets/wind-badge.svg" alt="Wind" width="128">

  # Wind

  A lightweight fullscreen magnifier for Windows - "light as air".

  [![Windows](https://img.shields.io/badge/Windows-10%20%7C%2011-0078D4?style=flat-square)](https://github.com/Maxaubert/Wind)
  [![Built with](https://img.shields.io/badge/C%2B%2B-Direct3D%2011-00599C?style=flat-square)](https://github.com/Maxaubert/Wind)
</div>

---

A replacement for the built-in Magnifier, with smooth continuous zoom that keeps tracking the
mouse even when games hide, clip, or center-lock the cursor.

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
- **Real cursor** at a constant on-screen size - including the text I-beam and link hand.
- **HDR-aware** - on an HDR display it tonemaps to match the desktop automatically (tracking
  the live SDR-brightness slider); on SDR it's a straight passthrough. No per-machine tuning.
- **Follows the mouse even when a game locks/hides the cursor** (HID-level Raw Input, no
  injection - anti-cheat safe).
- **Inspect mode** - freeze the cursor (keeps a hover/tooltip alive) and free-look around with
  a crosshair; clicks land where you aim.

## Magnifier models (`model=`)
Selected with the `model` ini key or the model row in Settings ("Magnifier model"). `model` is
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
  stepping by `magnifyStep` percent per notch (Settings > Display > Zoom step; lower = smoother
  and slower, applies live). Everything else is pure native Magnifier behavior; quitting Wind
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

## Build
Requires Visual Studio 2022+ Build Tools (Desktop development with C++). From any shell:
- `build.bat` - builds `Wind.exe` (runs from anywhere).
- `build.bat test` - builds and runs the unit tests.
- `build.bat uiaccess` - builds the UIAccess variant (signed-install prerequisite).
- `build.bat config` - builds the Settings app (`WindConfig.exe` + the Svelte UI).

## Install (signed build)
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
The Settings app (tray -> Open Settings) is the comfortable way to edit this file; it shows
only the rows that apply to the selected model and closes itself if the magnifier exits.
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
- Advanced: `zorderBand`, `transformExclude`, `noSwallowApps`, `profile`.

## Scope
Primary monitor by default (`multiMonitor=1` follows the cursor's monitor). Covers the desktop,
normal apps, and **borderless / windowed-fullscreen** games. Exclusive-fullscreen games are out
of scope (set the game to borderless).
