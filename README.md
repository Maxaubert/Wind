# Wind

A lightweight fullscreen magnifier for Windows - "light as air". A replacement for
the built-in Magnifier, with smooth continuous zoom that keeps tracking the mouse
even when games hide, clip, or center-lock the cursor.

## Controls (default, configurable in `magnifier.ini`)
- Hold **mouse forward button (XButton2)** - zoom in (smooth ramp).
- Hold **mouse back button (XButton1)** - zoom out (smooth ramp).
- Release - zoom stays at the current level.

## Build
Requires Visual Studio 2022+ Build Tools (Desktop development with C++). Run
`build.bat` from any shell.
- `build.bat` - builds `Wind.exe`.
- `build.bat test` - builds and runs unit tests.

## Run
`Wind.exe` - runs from the system tray. Right-click the tray icon to edit config or quit.

## Scope
v1 covers the desktop, normal apps, and **borderless / windowed-fullscreen** games.
Exclusive-fullscreen games are out of scope for v1 (set the game to borderless).
