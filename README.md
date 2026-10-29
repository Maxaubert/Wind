<div align="center">

  # Wind for Mac

  Fullscreen magnifier for macOS with smooth zoom that keeps up with your mouse.

  [![macOS 14+](https://img.shields.io/badge/macOS-14%2B-5b5bd6?style=flat-square&logo=apple&logoColor=white)](#install)
  [![Swift 6](https://img.shields.io/badge/Swift-6-5b5bd6?style=flat-square&logo=swift&logoColor=white)](Package.swift)
  [![Status: early](https://img.shields.io/badge/status-early-5b5bd6?style=flat-square)](#limits)

  The Mac version of [Wind](https://github.com/Maxaubert/Wind).
</div>

Wind zooms inside the macOS compositor, the same engine the built-in Zoom uses, so there is no
capture delay, protected video stays visible, and clicks land where you see them. Unlike the
built-in Zoom it keeps the pointer at its normal speed while zoomed, and it keeps tracking the
mouse when a game locks the pointer.

> [!WARNING]
> Wind for Mac uses private macOS functions (SkyLight) to drive the zoom. It works on macOS 27.2
> today, and a macOS update could break it.

## Features

- Smooth continuous zoom: hold a key or a mouse side button, or use a modifier plus the scroll wheel or trackpad.
- The pointer moves as fast across the desktop at 10x as at 1x (macOS's own Zoom slows it down by the zoom level).
- The view stays centred on the pointer, or moves only near the edges.
- Keeps panning from mouse movement when a game locks the pointer.
- Follows the text caret as you type (and, optionally, keyboard focus), steady even in terminal apps.
- Inspect mode: freezes the pointer so a tooltip stays open, pans the view with the mouse and shows a crosshair.
- Quick zoom toggle, keyboard panning, warmth and brightness filter.
- A Settings window, Open at login.
- Detects macOS Zoom's shortcuts, which fight Wind, and switches them off in one click.
- A crash watchdog that puts the screen back to 1x if Wind dies.

## Install

```sh
scripts/build.sh install
```

This builds `Wind.app`, copies it to `/Applications` and starts it. It needs the Xcode Command
Line Tools (`xcode-select --install`).

On first launch, macOS asks for the **Accessibility** permission. Turn Wind on in System
Settings > Privacy & Security > Accessibility; Wind starts by itself once it is on.

## Usage

| Action | Default |
|---|---|
| Zoom in (hold) | mouse forward button, or Ctrl+Opt+= |
| Zoom out (hold) | mouse back button, or Ctrl+Opt+- |
| Wheel zoom | Ctrl + scroll wheel or two-finger scroll |
| Quick zoom (toggle 1x and last level) | Ctrl+Opt+Z |
| Pan with the keyboard | Ctrl+Opt+arrows |
| Inspect mode | Ctrl+Opt+I |
| Quit Wind | Ctrl+Opt+Esc |

Bound keys and buttons are not passed on to the app you are using.

## Settings

Open **Settings…** from the menu-bar icon: zoom, mouse, tracking, binds and macOS tabs. The menu
itself holds the common switches. Every setting is a key in
`~/Library/Application Support/Wind/config.json`; the file reloads live when you save it. The log
is `~/Library/Logs/Wind/wind.log`.

Binds are lists of text like `"ctrl+opt+equal"`, `"mouse4"` or `"f13"`. Wind refuses binds that
would break typing, such as a letter key without Ctrl, Opt or Cmd.

## Limits

- Zooms the main display only.
- macOS Zoom must stay off: its shortcuts drive the same zoom and the two fight. Wind warns
  you (⚠︎ in the menu bar) and can switch them off for you.
- Keyboard binds do not work while a password field has secure input on (a macOS rule).
- The colour filter has not been checked on every display type.

## Development

```sh
swift run wind-checks          # unit checks for the pure logic (Sources/WindCore)
scripts/build.sh install       # build, install and relaunch
python3 scripts/e2e.py         # end-to-end checks against the installed app (the screen zooms)
python3 scripts/e2e_mouse.py   # pointer checks with real HID motion (needs Karabiner-Elements, sudo);
                               # the game-lock check needs an unlocked screen
```

How the macOS zoom behaves, and why Wind does what it does, is in
[docs/architecture.md](docs/architecture.md).
