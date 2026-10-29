# Wind for Mac

macOS fullscreen magnifier (Swift 6, SwiftPM, AppKit menu-bar agent) driving WindowServer's zoom
through private SkyLight calls. The Mac version of Wind (`~/Documents/Claude/Github/Wind`, Windows).
How the macOS zoom behaves and why: `docs/architecture.md`. Keep it in step with the code.

## Commands
- `swift run wind-checks`: unit checks for `Sources/WindCore` (no XCTest/swift-testing in CLT).
- `scripts/build.sh` builds `build/Wind.app`; `scripts/build.sh install` installs to /Applications
  and relaunches. Signs with the local "Wind Mac Dev" identity (keychain
  `~/Library/Keychains/windmac-dev.keychain-db`, password in the login keychain) so the Accessibility grant survives rebuilds.
- `scripts/build.sh test`: `build/test/Wind.app` with the e2e command channel compiled in.
- `python3 scripts/e2e.py`: e2e checks; builds and runs the test app in place of the installed one, restores it after.
- `python3 scripts/e2e_mouse.py`: real-HID pointer e2e via `tools/vmouse` (Karabiner, sudo).
- Run all three after touching `Magnifier.swift`, `SkyLight.swift` or `CaretTracker.swift`.

## Gotchas
- Never warp the pointer, rewrite event locations or use `CGAssociateMouseAndMouseCursorPosition`
  while zoomed: the zoom reinterprets them and the pointer snaps. Move it with `placePointer`
  (a `keepPointer=false` view write). Warps are fine at 1x only (tests warp before zooming in).
- Normal zoom writes use `keepPointer=true`; `false` makes a pointer-following view run into the corner.
- Measure pointer movement from where the pointer really landed (placement snaps to a grid).
- `screencapture` shows the unzoomed desktop; verify the view with `SLSGetZoomParameters` readback.
- macOS Zoom's shortcuts must stay off (issue #1); its prefs are not writable, use `SettingsToggle`.
- The command channel lets any same-user process drive Wind's Accessibility grant: it is compiled
  only into test builds (`#if WIND_TEST_CHANNEL`). Never compile it into the installed app.
- The crash guard runs as `Contents/MacOS/WindGuard` (a copy of the binary) so `pkill -x Wind`
  does not kill it.
- Tests zoom the real screen; the owner allowed that for this machine.
