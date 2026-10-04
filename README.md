<div align="center">
  <img src="assets/wind-badge.svg" alt="Wind logo" width="128">

  # Wind

  Fullscreen magnifier for Windows.

  [![Latest release](https://img.shields.io/github/v/release/Maxaubert/Wind?style=flat-square&color=5b5bd6&label=release)](https://github.com/Maxaubert/Wind/releases/latest)
  [![Windows 10 | 11](https://img.shields.io/badge/Windows-10%20%7C%2011-5b5bd6?style=flat-square)](https://github.com/Maxaubert/Wind/releases/latest)
  [![Licence: proprietary](https://img.shields.io/badge/licence-proprietary-5b5bd6?style=flat-square)](LICENSE)

  [Download](https://github.com/Maxaubert/Wind/releases/latest/download/Wind-Setup-x64.exe) · [Releases](https://github.com/Maxaubert/Wind/releases) · [Developer docs](docs/architecture/README.md)
</div>

https://github.com/user-attachments/assets/59939cd8-8bf3-4fbf-b978-8e89a4ebde1f

Wind replaces the built-in Magnifier with smooth, continuous zoom. It keeps tracking the mouse when
a game hides, clips or center-locks the cursor, and clicks pass through to the app under it.

## Features

- Smooth sub-pixel zoom and pan.
- Clicks, hover and dragging keep working while zoomed.
- Tracks the mouse in games through raw input, without injecting into the game.
- Auto picks the best engine for each window and switches when you alt-tab.
- The real cursor shapes, sharp at every zoom level.
- HDR aware: the zoomed view matches the desktop's brightness.
- Follows the text caret as you type, and optionally the keyboard focus.
- Inspect mode: freeze the pointer to keep a tooltip open and look around with a crosshair.
- Named profiles for different setups.
- Warmth and brightness filter for the whole screen.

## Install

[Download the installer](https://github.com/Maxaubert/Wind/releases/latest/download/Wind-Setup-x64.exe)
and run it. It always points at the latest release.

- Needs 64-bit Windows 10 or 11 and administrator rights.
- Installs to `C:\Program Files\Wind`. Windows grants the UIAccess permission Wind uses only to apps
  in that kind of protected location.
- Adds the WebView2 runtime if Settings needs it.
- Settings, profiles and logs stay in `%LOCALAPPDATA%\Wind`, also after an uninstall unless you
  choose otherwise.

The installer is not signed, so SmartScreen warns on first run: choose **More info**, then **Run
anyway**. Some browsers and work computers block the download. Setup signs Wind locally for your PC
so its zoom keys keep working over elevated windows; if that step fails, it installs a build without
that ability.

## Usage

The first launch walks you through choosing zoom keys: mouse side buttons, keyboard keys, or both.

- **Hold** the zoom-in key to zoom in, the zoom-out key to zoom out. Release to stay at that level.
- **Wheel zoom:** bind a modifier plus the scroll wheel (for example Ctrl+wheel).
- **Keyboard panning:** optional arrow-key binds move the view while zoomed.
- **Quick zoom:** Ctrl plus a zoom key toggles between 1x and your last level.
- **Inspect mode:** an optional key freezes the pointer and lets you look around with a crosshair.
- **Ctrl+Alt+Q** quits Wind from anywhere.

Bound keys are not passed on to the app you are using. Wind refuses binds that would break normal
typing or Windows shortcuts and tells you why.

## Settings

Open them from the tray icon > Settings. Changes apply immediately; Save keeps them in the current
profile. Preferences > Show advanced settings shows the rest. Every setting is also a key in
`%LOCALAPPDATA%\Wind\magnifier.ini`; see the
[key reference](docs/architecture/08-config-profiles.md#key-reference).

## Limits

- Magnifies the primary monitor unless `multiMonitor=1` is set.
- Games must run borderless or windowed. Switch exclusive-fullscreen games to borderless.
- The Start menu, taskbar and tray flyouts are not magnified by default.
- Protected video (Netflix and similar) shows normally in Auto, which switches to the transform
  engine for it. Forcing the Render engine shows it black.

## Development

`build.bat` builds Wind, `build.bat test` runs the unit tests, and `build.bat config` builds the
Settings app. The developer docs are in [docs/architecture](docs/architecture/README.md).

## Licence

Wind is proprietary: the source code may not be used, copied, modified or redistributed without
written permission, and the official binaries are free to install and use. Releases up to v0.6.1
were MIT-licensed and stay so. See [LICENSE](LICENSE) and
[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md). Commercial licensing: aubert@post.com
