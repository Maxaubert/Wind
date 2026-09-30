# Tinted pointer at 1x (issue #288 follow-up)

Date: 2026-09-30. Owner: Max. Status: approved to build without review ("plan spec and implement now,
no approval from me"); owner tests the verified build.

## Problem

Warmth and Brightness reach everything at 1x except the mouse pointer. Windows draws the pointer on a
hardware cursor plane after composition, which the DWM colour effect never touches (Night light, in
the display pipeline, does). While zoomed Wind draws the cursor itself, so it is filtered there.

## Design

While the colour is not neutral, Wind is idle (1x, not Inspect), the engine is not the native
Magnifier, and no fullscreen game is in front, Wind replaces the standard system pointers with tinted
copies (`SetSystemCursor`). They stay hardware pointers: no per-frame cost, no latency.

- **Source images:** pristine copies of the 14 standard pointers captured at start-up (after the
  start-up scheme reload), per-monitor-DPI sized (64x64 at 225% on the owner's PC), recaptured when
  the user changes the pointer scheme (`WM_SETTINGCHANGE` + `SPI_SETCURSORS`).
- **Tint:** the same matrix as the screen in sRGB-encoded form (`BuildColorMatrix(w, d, false)`), applied
  to each pixel's RGB, alpha kept. Size and hotspot unchanged.
- **Monochrome pointers** (the default text beam inverts what is under it): rebuilt as colour
  pointers: black/white pixels kept, inverting pixels drawn white with a 1 px dark outline so the beam
  stays visible on light and dark backgrounds, then tinted.
- **Animated pointers** (busy, app starting) are left as they are: a static copy would stop the
  animation.
- **Zoom-in:** the pristine pointers go back first, before the engine hides or captures the pointer
  (the transform sprite and the render engine draw the real shape and filter it themselves; a tinted
  source would be tinted twice). Direct `SetSystemCursor` of the in-memory copies, never a scheme
  reload (`SPI_SETCURSORS` rereads the registry and broadcasts to every window: a hitch risk).
- **Zoom-out:** the engines restore the scheme; the next idle tick re-applies the tint.
- **Fullscreen game in front:** pristine pointers (the DWM effect does not reach exclusive fullscreen,
  so a tinted arrow would not match). Checked at most every 250 ms while idle.
- **Exit, crash, force-kill:** quitting restores the pristine pointers; the existing start-up and crash
  heals (`SPI_SETCURSORS`) restore the scheme, so a force-killed Wind is healed on its next start.
- **No fighting:** Wind only rewrites pointers when its colour, zoom state or the foreground-game state
  changes, never on a loop.

## Testing

- Doctests: pixel tint (RGB by matrix, alpha kept, identity untouched), monochrome conversion (the
  four AND/XOR cases and the outline).
- Field verification on the owner's PC (by Claude, before the owner tests): tinted system pointers
  read back with the expected pixels, size and hotspot; the pointer shape Desktop Duplication reports
  is the tinted one; zoom-in restores pristine and zoom-out re-tints; a borderless fullscreen window
  in front restores pristine; quitting restores; apply/restore timing logged.
