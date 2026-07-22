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
and scaling it on the GPU (Direct3D 11) onto a click-through overlay. That gives sub-pixel
smooth panning and a crisp centered cursor that the integer-offset Windows Magnification API
can't, and lets you keep clicking and using the screen while zoomed.

## Features
- **Smooth, sub-pixel zoom and pan** with light inertia - no stepping or cursor hop.
- **Interact while zoomed** - clicks pass through to the app under the centered cursor.
- **Covers the whole screen, including the Start menu, taskbar, and tray flyouts** (requires
  the signed UIAccess install, below).
- **Real cursor**, drawn centered - including the text I-beam and link hand.
- **HDR-aware** - on an HDR10 display it tonemaps to match the desktop automatically; on SDR
  it's a straight passthrough. No per-machine tuning.
- **Follows the mouse even when a game locks/hides the cursor** (HID-level Raw Input, no
  injection - anti-cheat safe).

## Magnifier models (`model=`)
Wind ships two magnifier implementations. They are selected with the `model` key, or from the
model row in Settings. `model` is read once at launch, so **switching it restarts Wind** (Settings
prompts you to confirm).

- **`render`** (default) - captures the desktop with DXGI Desktop Duplication and redraws it into
  a D3D11 overlay. The cursor is drawn into the same frame as the content, so it can never drift
  against the view. This is the model every knob below was tuned for, and the only one that covers
  the shell (Start / taskbar / tray) via the UIAccess z-order band.
- **`magnify`** - drives the **native Windows Magnifier** with Wind's controls. This is the model
  for DRM-protected video (Netflix and friends), which shows black under `render`'s screen
  capture. Wind preps Magnifier's settings (fullscreen mode, small zoom steps, toolbar
  minimized), starts it on the first zoom-in, and paces injected `Win`+`Plus`/`Minus` presses so
  your side-button hold-to-zoom, ramp, and quick-zoom work as usual. Windows Magnifier can only
  zoom in steps (that is its hard limit), so `magnify` steps by `magnifyStep` percent (5% by
  default - near-smooth) instead of scaling continuously. At 1x Magnifier stays running,
  invisible, for an instant next zoom-in; quitting Wind (or swapping models) closes it and
  restores your original Windows Magnifier settings.

The `magnify` model hands the view and cursor to Windows Magnifier, so the render-only features
do not apply there: `sharpness`, `hdrTonemap`, `bilinear`, `outline*`, `brightness`,
`cursorSensitivity`/`cursorSmoothing`, `multiMonitor`, and Inspect mode.

## Controls (defaults, configurable in `magnifier.ini`)
- Hold **mouse forward button (XButton2)** - zoom in (smooth ramp).
- Hold **mouse back button (XButton1)** - zoom out.
- Release - zoom stays at the current level.
- **Magnifier model swap** (unbound by default) - press to toggle between the `render` and `magnify` magnifier models; Wind restarts onto the other model. Bind it in Settings under Display -> Magnifier model.
- **Ctrl+Alt+Q** - quit from anywhere (also restores the cursor); or use the tray icon.

## Build
Requires Visual Studio 2022+ Build Tools (Desktop development with C++). From any shell:
- `build.bat` - builds `Wind.exe` (runs from anywhere; covers everything except the shell flyouts).
- `build.bat test` - builds and runs the unit tests.
- `build.bat uiaccess` - builds the UIAccess variant (needed to cover Start/taskbar/tray).

## Install (for Start menu / taskbar / tray coverage)
Covering the shell needs UIAccess, which requires a code-signed binary run from a secure
location. Run **elevated**:
```
powershell -ExecutionPolicy Bypass -File tools\uiaccess_setup.ps1
```
This builds the UIAccess variant, self-signs it, installs it to `C:\Program Files\Wind`, and
writes a default `magnifier.ini`. Launch `C:\Program Files\Wind\Wind.exe` from a normal
(non-elevated) window so UIAccess engages.

## Config (`magnifier.ini`, hot-reloads where noted)
- `zoomInButton`/`zoomOutButton` (mouse side-buttons) and `zoomInVk`/`zoomOutVk` (keyboard,
  default PageUp/PageDown) - hold to zoom in/out.
- `cursorSensitivity`, `cursorSmoothing` - pan speed and inertia.
- `maxLevel`, `fullRangeSeconds` - max zoom and ramp time.
- `bilinear`, `cursorScaleWithZoom`, `cursorVisibility` - smoothing, cursor scaling, when to draw the cursor.
- `brightness` - optional output fine-tune (hot-reloads).
- Pacing/perf: `dwmFlush` (default on, smooth), `vsync`, `tickHzCap` (0 = auto-detect refresh).
- `model` - `render` (default) or `magnify`. See **Magnifier models** above. Restart to switch.
- Magnify-only: `magnifyStep` (Windows Magnifier percent per step: 5/10/25/50; default 5).
- Advanced: `zorderBand`, `hdrTonemap`.

The Settings app (tray -> Open Settings) writes the same `magnifier.ini`, shows only the rows that
apply to the selected model, and closes itself if the magnifier exits.

## Scope
Single primary monitor. Covers the desktop, normal apps, and **borderless / windowed-fullscreen**
games. Exclusive-fullscreen games are out of scope (set the game to borderless).
