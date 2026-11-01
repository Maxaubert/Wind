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
- Follows the text caret as you type (and, optionally, keyboard focus), tested in TextEdit, Terminal, Safari, Chrome and Claude Code.
- Inspect mode: hides and freezes the pointer so a tooltip stays open; the mouse moves a crosshair and the view, which never jumps on entering or leaving.
- Quick zoom toggle, keyboard panning, warmth and brightness filter.
- A Settings window, Open at login.
- Detects macOS Zoom's shortcuts, which fight Wind, and switches them off in one click (releasing macOS Zoom if it is stuck zoomed in).
- A crash watchdog that puts the screen back to 1x if Wind dies.

## Install

```sh
scripts/build.sh install
```

This builds `Wind.app`, copies it to `/Applications` and starts it. It needs the Xcode Command
Line Tools (`xcode-select --install`).

On first launch, macOS asks for the **Accessibility** permission. Turn Wind on in System
Settings > Privacy & Security > Accessibility; Wind starts by itself once it is on. The first time a
game takes the pointer, Wind also asks for **Input Monitoring**, which it needs to follow the mouse in games.

## Usage

| Action | Default |
|---|---|
| Zoom in and out | Ctrl + scroll wheel or two-finger scroll (each notch is one step; speed in Settings) |
| Zoom in or out by holding a key or button | none: pick them in the first-run setup or Settings (up to three each) |
| Pan with the keyboard | Ctrl+Opt+arrows |
| Quit Wind | Cmd+Esc |
| Quick zoom, Inspect mode, Hide pointer | none: set one key each in Settings |

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
- For caret tracking, Wind turns on Chromium browsers' full accessibility mode (`AXEnhancedUserInterface`).

## Development

```sh
swift run wind-checks          # unit checks for the pure logic (Sources/WindCore)
scripts/build.sh install       # build, install and relaunch
python3 scripts/e2e.py         # end-to-end checks against the installed app (the screen zooms)
python3 scripts/e2e_mouse.py   # pointer checks with real HID motion (needs Karabiner-Elements, sudo);
                               # the game-lock check needs an unlocked screen
python3 scripts/e2e_caret.py   # caret following with real typing in five apps
python3 scripts/e2e_syszoom.py # the macOS Zoom clash and Wind's fix (opens System Settings)
```

How the macOS zoom behaves, and why Wind does what it does, is in
[docs/architecture.md](docs/architecture.md).
