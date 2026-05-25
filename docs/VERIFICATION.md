# Wind manual verification

Build with `build.bat`, then run `Wind.exe` (it copies/creates `magnifier.ini` next
to it on first run). Wind sits in the system tray. Then verify:

**Safety:** press **Ctrl+Alt+Q** anytime to quit cleanly (restores the cursor + unzooms),
even while the render overlay covers the screen. The tray right-click -> Quit also works.

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

## Own GPU renderer (engine=render, the default)

The own capture+Direct3D renderer (DXGI Desktop Duplication). Select with `engine=render`
in magnifier.ini (default); `engine=mag` selects the Magnification-API engine.

**Auto-verified (CI/dev, via render-then-dump PNGs):**
- D3D11 device + click-through overlay + flip-swapchain present.
- Desktop Duplication capture (cursor excluded; overlay excluded from capture via
  WDA_EXCLUDEFROMCAPTURE so we don't magnify our own output).
- Sub-pixel float source-rect magnify shader (bilinear).
- Real cursor decoded (GetCursorInfo) and drawn centered, alpha-blended, scaled by zoom.
- Cursor hide + SetCursorPos click-sync + clean shutdown (cursor restored).
- End-to-end: `WIND_SELFTEST=1 Wind.exe` drives the real path and dumps `wind_selftest.png`.

**Human-only checks (please verify when you return):**
- [ ] Zoom in (engine=render): exactly ONE cursor visible (not two). If two, the OS-cursor
      hide needs the documented fallback (see KNOWN-ISSUES "Own renderer").
- [ ] Pan while zoomed: cursor stays centered and BUTTER SMOOTH (no L-pixel hop) - the goal.
- [ ] Content pans smoothly at high zoom (8x) - no judder.
- [ ] Click something while zoomed: it lands where the centered cursor points.
- [ ] DRM video (e.g. Netflix) shows BLACK in the magnified layer (known DDA limit).
- [ ] Quit from tray: cursor + screen back to normal everywhere.
- [ ] A/B vs engine=mag and vs Windows Magnifier for smoothness/feel.

## Notes / known v1 behavior
- Editing the config while running re-initializes zoom to 1.0x (rare action).
- Renderer knobs (cursorSensitivity, cursorScaleWithZoom, bilinear) apply on restart.
- v1 magnifies the primary monitor; SDR; desktop-focused (engine=mag still serves games).
- Recenter is unbound by default (recenterVk=0); no keyboard hook is wired in v1.
