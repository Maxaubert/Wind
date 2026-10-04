Fullscreen magnifier for Windows with smooth zoom that keeps tracking the mouse in games.

## Install

Download **Wind-Setup-x64-__VERSION__.exe** below and run it (`Wind-Setup-x64.exe` is the same
installer under a name that always points at the latest release). Setup needs 64-bit Windows 10 or
11 and administrator rights, installs to `C:\Program Files\Wind`, offers to start Wind when you
sign in, and adds the WebView2 runtime if it is missing. Settings, profiles and logs stay in
`%LOCALAPPDATA%\Wind`; uninstalling keeps them unless you choose otherwise.

## Unsigned installer

The installer is not signed, so SmartScreen warns on first run: choose **More info**, then
**Run anyway**. Some browsers and managed work computers block the download. During setup, Wind is
signed locally with a certificate created for your PC, whose private key is deleted straight away;
this lets zoom keys work over elevated windows. If that step fails, setup installs a build without
that ability.

## Verify

SHA-256 of `Wind-Setup-x64-__VERSION__.exe` (and its copy `Wind-Setup-x64.exe`):

```
__SHA256__
```

Built from `__COMMIT__`.
