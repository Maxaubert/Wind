# Wind for Mac

macOS fullscreen magnifier (Swift 6, SwiftPM, AppKit menu-bar agent) driving WindowServer's zoom
through private SkyLight calls. The Mac version of Wind (`~/Documents/Claude/Github/Wind`, Windows).
How the macOS zoom behaves and why: `docs/architecture.md`. Keep it in step with the code.

## Commands
- `swift run wind-checks`: unit checks for `Sources/WindCore` (no XCTest/swift-testing in CLT).
- `scripts/build.sh` builds `build/Wind.app`; `scripts/build.sh install` installs to /Applications
  and relaunches. Signs with the local "Wind Mac Dev" identity (keychain
  `~/Library/Keychains/windmac-dev.keychain-db`; its password is the login keychain item `windmac-dev-keychain`) so the
  Accessibility grant survives rebuilds. Without that keychain it signs ad hoc.
- `scripts/release.sh`: the public release zip (`dist/Wind-mac-<version>.zip`, ad hoc signed, licence inside, refuses a
  build with the test channel). `.github/workflows/mac.yml` runs the checks on PRs and publishes `mac-v<VERSION>` on main
  (never the repo's latest release). Bump `VERSION` in the PR, like the Windows version.
- `scripts/build.sh test`: `build/test/Wind.app` with the e2e command channel compiled in.
- App icon: `assets/Wind.icon` (Icon Composer) compiled by `scripts/make_icon.sh` (needs Xcode) into the
  committed `assets/icon/` (Assets.car: no grey plate on macOS 26+; Wind.icns for older macOS).
- Settings UI (`ui/`, Svelte): `npm ci && npm run build`, `npx playwright test` (mock bridge, WebKit +
  Chromium), `npm run test:rules` (keybind rules and config defaults). Design: `docs/architecture.md`, Settings window.
- `python3 scripts/e2e_settings.py`: the settings window in the test build (page writes, keybinds, setup).
- `python3 scripts/e2e_settings_mouse.py`: the settings window ON SCREEN with real clicks and keys (select list, slider arrows).
- `python3 scripts/e2e.py`: e2e checks; builds and runs the test app in place of the installed one, restores it after.
- `python3 scripts/e2e_mouse.py`: real-HID pointer e2e via `tools/vmouse` (Karabiner, sudo).
- `python3 scripts/e2e_caret.py`: real typing in TextEdit, Terminal, Safari, Chrome, Claude Code
  (unlocked screen; types only into its own WINDTEST window, never presses Enter).
- `python3 scripts/e2e_syszoom.py`: macOS Zoom clash and Wind's fix (opens System Settings, leaves
  every macOS Zoom shortcut off).
- Run all three after touching `Magnifier.swift`, `SkyLight.swift` or `CaretTracker.swift`.
- `python3 scripts/e2e_focus.py`: keyboard focus tracking in a throwaway Chrome (WIND_ENGINE=draw for render).
- `python3 scripts/e2e_draw.py`: the render engine (`engine = "draw"`; Transform, `"zoom"`, is the default since 2026-11-02); `WIND_ENGINE=draw`
  runs any suite on it. Both start the test build directly so it inherits Screen Recording from the terminal.
  `WIND_CURSOR=wind` runs any suite with Wind's own cursor.
- Render scaling (`renderScaling`: Nearest, Smooth, Sharp, NIS, xBR; design in `docs/architecture.md`, Render scaling):
  `e2e_draw.py` checks each on screen against `tools/upscaleref` (the Windows HLSL); `python3 scripts/scaling_truth.py`
  measures GPU, CPU and pan pacing per filter at 3x and 6x. `WIND_SCALING=N` runs any suite with that filter.
  NIS and xBR draw into a CAMetalLayer: never a new IOSurface as layer contents per frame (WindowServer skips refreshes).
- `python3 scripts/pan_truth.py`: stutter while zoomed on the zoomed screen (pan, stop, pan again; zooming
  further); every movement start must be as smooth as its middle.
- `python3 scripts/handover_truth.py`: panning with a system alert up on the zoomed screen (Transform, Render drawing
  it, `--states zoomalert,handover --tier 250` the hand-over to macOS's zoom); run it after touching `DrawZoom` prompts or the capture.
- `python3 scripts/edge_truth.py`: the pinned view at screen edges on the zoomed screen (edge shake).
- `python3 scripts/wobble_from_log.py [HH:MM:SS]`: cursor wobble from a real session in the log (logLevel 2).
- `python3 scripts/e2e_screenshot.py`: screenshot mode (area, window, Cmd+Shift+5) and the colour sampler while
  zoomed, both engines; clicks and Escape only while screencapture or the sampler holds the pointer.
  It also covers Wind's own selection (Cmd+Shift+4 while zoomed: saved rect exact, Option, Shift, Escape,
  clipboard, window mode, native at 1x and with `ownScreenshot` off); `python3 scripts/drawshot_truth.py`: its smoothness on the zoomed screen.
  Both point the owner's `location-screenshot`/`show-thumbnail` at build/ and restore them (never the Desktop).
- `python3 scripts/e2e_hover.py`: hover in other apps while zoomed (a notification banner, SwiftUI and AppKit
  hover, click-through), every engine and cursor, plus the cursor given back at 1x; run it (and `e2e_draw.py`'s
  Dock, app-switch and prompt checks) after touching how Wind hides the real cursor or `AlwaysOwnsCursor`.
- `python3 scripts/cursor_truth.py`: Wind's cursor and Inspect's crosshair measured on the ZOOMED screen
  (what the owner sees, frame by frame); run it after touching either or the write order.
- `python3 scripts/screenshot_truth.py`: macOS screenshot mode on the ZOOMED screen (crosshair wobble, band lag, pacing); run it after touching the still view in screenshot mode.

## Gotchas
- Never warp the pointer, rewrite event locations or use `CGAssociateMouseAndMouseCursorPosition`
  while zoomed: the zoom reinterprets them and the pointer snaps. Move it with `placePointer`
  (a `keepPointer=false` view write). Warps are fine at 1x only (tests warp before zooming in).
- Normal zoom writes use `keepPointer=true`; `false` makes a pointer-following view run into the corner.
- Test builds log to `~/Library/Logs/Wind/test/`; the installed app to `~/Library/Logs/Wind/` (tests no longer rotate the owner's log away).
- Measure the hand from ONE pointer read per tick against Wind's own computed carry; never from a
  read taken right after a zoom write (it can be off by a whole point, or tens of points near edges).
- `scripts/explore.py` stress-tests edges, corners and zooming with the virtual mouse; run it after
  touching placement (report in `build/explore-report.txt`).
- While a game holds the pointer, pan only from raw HID counts (`RawMouse`): tap deltas then
  include Wind's own echoes and run away. Game lock = cursor hidden while the mouse moves.
- `scripts/build.sh` fails on a compile error. Never mask it: a stale binary passed tests before.
- `screencapture` shows the unzoomed desktop; verify the view with `SLSGetZoomParameters` readback.
  An SCK stream with the cursor that excludes only a windowless process shows the zoomed screen.
- A window moved with `setFrame` reaches the screen 1-2 frames after a zoom write: anything drawn
  to match the zoom is a layer in a window that never moves, moved before every view write.
- macOS Zoom's shortcuts must stay off (issue #1); its prefs are not writable, use `SettingsToggle`.
- The command channel lets any same-user process drive Wind's Accessibility grant: it is compiled
  only into test builds (`#if WIND_TEST_CHANNEL`). Never compile it into the installed app.
- The crash guard runs as `Contents/MacOS/WindGuard` (a copy of the binary) so `pkill -x Wind`
  does not kill it.
- Tests zoom the real screen; the owner allowed that for this machine.
