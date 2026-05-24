# Wind manual verification

Build with `build.bat`, then run `Wind.exe` (it copies/creates `magnifier.ini` next
to it on first run). Wind sits in the system tray. Then verify:

## Desktop
- [ ] Hold forward (XButton2): screen zooms in smoothly (no steps), follows the cursor.
- [ ] Hold back (XButton1): zooms out smoothly; stops at 1.0x (screen back to normal).
- [ ] Release mid-zoom: level stays put.
- [ ] Move the mouse while zoomed: the lens follows the cursor.
- [ ] Quit from the tray (right-click -> Quit): screen returns to 1x (never left zoomed).
- [ ] Edit magnifier.ini (set maxLevel=4.0), save: new max applies within ~1s.
- [ ] Tray right-click -> "Edit config" opens magnifier.ini in Notepad.

## In a borderless-fullscreen game (cursor hidden / center-locked)
- [ ] Hold forward: the game view zooms in.
- [ ] Move the mouse: the lens PANS even though the game hides/locks the cursor
      (this is the core feature - Raw Input driving the lens).
- [ ] The forward/back side buttons do not trigger anything unexpected in-game.

## Performance
- [ ] Task Manager: Wind CPU stays near 0% idle-zoomed; low while panning.
- [ ] No noticeable stutter added to the game.

## Notes / known v1 behavior
- Editing the config while running re-initializes zoom to 1.0x (rare action).
- v1 magnifies the primary monitor; borderless games only (not exclusive fullscreen).
- Recenter is unbound by default (recenterVk=0); no keyboard hook is wired in v1.
