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

## Installer (issue #213)

Spec `docs/superpowers/specs/2026-08-20-installer-design.md`, sources in `installer/`.

**Auto-verified** by `build.bat installer`, which compiles the script and then runs
`tools\installer_check.ps1`:
- every `File` source the script packs exists, `/nonfatal` ones included,
- every rectangle the screens read is present in the generated `over.nsh` (the failure a
  rename in `over.html` causes, which a compile does not catch),
- a silent install lands the payload, the ARP key and the Run value, and a silent uninstall
  removes all three and KEEPS `%LOCALAPPDATA%\Wind`.
  The install half needs an elevated shell and skips itself without one.

Also rig-verified once, by probe rather than by suite:
- the `Local\Wind_QuitRequest` handshake stops a running Wind on its own, mutex released
  after 3 ms, no `taskkill` needed,
- an install over a running Wind writes the clean-shutdown line
  (`MagUninitialize -> refs=0`) to `wind-core.log`, so the polite path really ran,
- launching through `explorer.exe` yields a NOT-elevated Wind where a plain launch from the
  same elevated context yields an ELEVATED one.

**The limit worth stating:** `/S` exercises the section, not the drawn UI, and the drawn UI
is most of the code. Three approaches to capturing the live window failed (`PrintWindow`
returns blank on the DIB-into-static drawing), so the screens below are human-only.

**Human-only checks:**
- [ ] Fresh install on a machine with no Wind: files in `C:\Program Files\Wind`, entry in
      Settings > Apps, Run value in Task Manager > Startup, tray icon after Finish.
- [ ] The window is frameless with rounded corners, centred, and the loop plays smoothly and
      wraps without a visible jump.
- [ ] Hover Install, Back, minimise and close: each one lights up, and the hit area matches
      what it looks like. Drag the caption strip: the window moves.
- [ ] The setup screen shows `C:\Program Files\Wind` in Consolas, in the gap left for it.
- [ ] Toggle "Start Wind when I sign in", go forward, come Back: the box kept its state.
- [ ] The progress bar sits ON the drawn trough, in Wind's indigo, not the Windows green.
- [ ] The done screen's two boxes toggle, and Finish opens Wind only when "Open Wind now" is
      ticked. The Wind it opens is NOT elevated (Task Manager > Details > Elevated column).
- [ ] Upgrade while Wind is running AND zoomed: no "file in use" error, and the OS cursor is
      visible afterwards. A stranded hidden cursor means the polite quit was skipped.
- [ ] Upgrade with the Settings window open: WindConfig closes, no orphan process.
- [ ] Uninstall, answer NO to removing settings: `%LOCALAPPDATA%\Wind\magnifier.ini` survives.
- [ ] Uninstall, answer YES: it does not.
- [ ] At 100% DPI and at 225%: window centred, type sharp, hit targets land where they look.
      225% loads the 1440 overlays, 100% loads the 960 set.
- [ ] Tray > Open Settings works after install (proves WebView2 is present or was installed).

## Notes / known v1 behavior
- Editing the config while running re-initializes zoom to 1.0x (rare action).
- Renderer knobs (cursorSensitivity, cursorScaleWithZoom, bilinear) apply on restart.
- v1 magnifies the primary monitor; SDR; desktop-focused (engine=mag still serves games).
- Recenter is unbound by default (recenterVk=0); no keyboard hook is wired in v1.
