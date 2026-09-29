A lightweight fullscreen magnifier for Windows. Smooth zoom that keeps tracking the mouse
even when a game hides or locks the cursor.

## Install

Download **Wind-Setup-x64-__VERSION__.exe** below and run it. Setup installs to
`C:\Program Files\Wind` and asks for administrator rights, offers to start Wind when you
sign in, and installs the WebView2 runtime if the Settings window has no browser engine to
run in. Your settings, profiles and logs live in `%LOCALAPPDATA%\Wind`, and uninstalling
keeps them unless you say otherwise.

Requires 64-bit Windows 10 or 11.

## One thing to know before you download

**This installer is unsigned.** SmartScreen will warn on first run: choose *More info* then
*Run anyway*. Some browsers and most managed work computers block the download outright,
which a signature is the only real fix for; one is being arranged.

That does not cost you UIAccess, though: Setup signs Wind for UIAccess on your own PC during
install, using a certificate it generates and trusts locally, then deletes right away - so
zoom shortcuts keep working with an elevated window focused (Task Manager, regedit, an
elevated terminal), and the desktop uses the same compositor transform engine as a game.
There is nothing to configure either way.

## What is in it

- Hold-to-zoom on the mouse side buttons, and configurable keybinds
- Keeps tracking the cursor in games that hide or lock it, using raw HID input
- Inspect mode: freeze the pointer and free-look around the magnified view
- Automatic engine choice per zoom, between a DWM fullscreen transform and its own
  DXGI + Direct3D 11 renderer
- Named settings profiles, and a Settings app with guided first-run setup
- Tracking modes: follow the text caret or the keyboard-focused control instead of the
  pointer, with a smooth glide, plus a mouse edge mode
- Multi-monitor and HDR aware

## Verify your download

SHA-256 of `Wind-Setup-x64-__VERSION__.exe`:

```
__SHA256__
```

---

Built from `__COMMIT__` by the release workflow. The installer on this page is rebuilt and
replaced on every push to `main`, so it always matches the current source.
