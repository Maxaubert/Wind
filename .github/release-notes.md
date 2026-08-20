A lightweight fullscreen magnifier for Windows. Smooth zoom that keeps tracking the mouse
even when a game hides or locks the cursor.

## Install

Download **Wind-Setup-x64-__VERSION__.exe** below and run it. Setup installs to
`C:\Program Files\Wind` and asks for administrator rights, offers to start Wind when you
sign in, and installs the WebView2 runtime if the Settings window has no browser engine to
run in. Your settings, profiles and logs live in `%LOCALAPPDATA%\Wind`, and uninstalling
keeps them unless you say otherwise.

Requires 64-bit Windows 10 or 11.

## Two things to know before you download

**This build is unsigned.** SmartScreen will warn on first run: choose *More info* then
*Run anyway*. A free open-source certificate is being sought from
[SignPath Foundation](https://signpath.org/).

**Being unsigned costs two features**, because Windows only grants UIAccess to a signed
binary in a protected folder:

- zoom shortcuts do not work while an elevated window has focus (Task Manager, regedit, an
  elevated terminal)
- the desktop transform path stays off, so the desktop is magnified by the render engine

Everything else works normally. Wind detects this at startup and picks the right engine on
its own, so there is nothing to configure.

## What is in it

- Hold-to-zoom on the mouse side buttons, and configurable keybinds
- Keeps tracking the cursor in games that hide or lock it, using raw HID input
- Inspect mode: freeze the pointer and free-look around the magnified view
- Automatic engine choice per zoom, between a DWM fullscreen transform and its own
  DXGI + Direct3D 11 renderer
- Named settings profiles, and a Settings app with guided first-run setup
- Multi-monitor and HDR aware

## Verify your download

SHA-256 of `Wind-Setup-x64-__VERSION__.exe`:

```
__SHA256__
```

---

Built from `__COMMIT__` by the release workflow. The installer on this page is rebuilt and
replaced on every push to `main`, so it always matches the current source.
