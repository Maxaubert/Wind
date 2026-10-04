# Release smoke checklist

Install the build under test, launch Wind from a normal (non-elevated) shell, and work through the
list. Ctrl+Alt+Q quits cleanly at any time. Settings live in `%LOCALAPPDATA%\Wind\magnifier.ini`.
`build.bat installer` already checks the installer's files, generated layout and a silent
install/uninstall round trip (`tools\installer_check.ps1`).

## Install and uninstall
- [ ] Fresh install: files in `C:\Program Files\Wind`, entry in Settings > Apps, Run value in Task Manager > Startup, tray icon after Finish.
- [ ] The setup window is frameless, centred, the loop plays and wraps without a jump; buttons light up on hover and hit where they look.
- [ ] Licence screen: Install stays disabled until the box is ticked; "Read the full licence" opens LICENSE.txt.
- [ ] The progress bar sits on the drawn trough in Wind's accent colour (#5b5bd6), not Windows green.
- [ ] Finish opens Wind only when "Open Wind now" is ticked, and that Wind is not elevated (Task Manager > Details > Elevated).
- [ ] Upgrade while Wind runs zoomed: no "file in use" error, the OS cursor is visible afterwards. Upgrade with Settings open: WindConfig closes.
- [ ] Uninstall keeping settings: `magnifier.ini` survives. Uninstall removing settings: it does not.
- [ ] At 100% and 225% DPI the setup window is sharp and its hit targets line up.

## Desktop zoom
- [ ] First launch runs the guided setup; the chosen binds work.
- [ ] Hold zoom-in: smooth ramp, no steps. Hold zoom-out: back to 1x. Release mid-zoom: the level stays.
- [ ] Wheel zoom with the chosen modifiers; plain scrolling is unaffected.
- [ ] Keyboard panning while zoomed; at 1x the keys reach the app.
- [ ] Clicks, hover, drag and text selection land under the cursor while zoomed.
- [ ] Ctrl+Alt+Q and tray Quit both leave the screen at 1x with a visible cursor.

## Game (borderless)
- [ ] Zoom in a game that hides or centre-locks the cursor: the view pans with the mouse.
- [ ] Clicks and drags in the game work while zoomed; no visible frame-rate drop while panning.

## Tracking
- [ ] Typing in Notepad, a browser and VS Code moves the view with the caret; a mouse move takes it back.
- [ ] Caret tracking in IntelliJ or PyCharm (Java Access Bridge).

## Colour
- [ ] Warmth and Brightness change the screen at 1x and zoomed in both engines; the pointer is tinted at 1x.
- [ ] Quitting Wind clears the filter.

## Settings
- [ ] Tray > Open Settings opens; a slider applies at once; Save and Discard behave; the engine row restarts Wind.
- [ ] Killing the WebView2 browser process: Settings recovers with unsaved edits kept.

## Tray flyout
- [ ] Opens from the icon, closes on Esc and outside click; sliders, toggles and the engine dropdown apply; Quit prompts when there are unsaved changes.

## Profiles
- [ ] Create, switch and delete a profile from Settings; switch from the tray; a profile with another engine restarts Wind.

## HDR
- [ ] With Windows HDR on, zooming in and out shows no brightness step, also after moving the SDR content brightness slider.

## DRM video
- [ ] Netflix or another protected stream in Auto: the magnified view shows the video, not black.
- [ ] With `model=render` the same video shows black (expected).
