# Keyboard panning (issue #287)

## 1. Goal
While zoomed, move the magnified view with the keyboard, without touching the mouse, like Windows
Magnifier's Ctrl+Alt+arrow keys. For reading long text and for keeping hands on the keyboard.

## 2. Owner decisions (2026-09-30)
- **Binds:** four settable keybinds, Pan left / right / up / down. Default **Ctrl+Alt+arrows**
  (bound out of the box, unlike the zoom binds), exactly like Windows Magnifier.
- **Feel:** **tap to nudge, hold to pan.** A tap moves the view one step; holding pans
  continuously with a short ease-in, and the motion glides out on release. One **Pan speed**
  slider in Settings. The same feel at every zoom level (defined in screen space).
- **Scope:** the four directions only. No reading helpers (line start / next line).
- **Only while zoomed:** at 1x the keys pass straight to the app (IntelliJ keeps Ctrl+Alt+Left/Right
  for navigate back/forward). Zoomed, Wind swallows them like the other keyboard binds.
- Issue #286 (per-app profiles) was dropped the same day; nothing here depends on it.

## 3. Behaviour
- **Press:** panning starts at once, easing in over ~150 ms to `panSpeed x 1.25` screen widths per
  second in EVERY direction (heights for up/down were 56% as fast on 16:9, owner test), scaled by
  zoom: full speed from 7.5x, `(level / 7.5)^0.6` below, one smooth curve (#305; piecewise-linear
  versions with a separate low-zoom cap did not feel proportional), and the tap
  step is 1/8 of the width for all four. AMENDED 2026-09-30 after the owner's first test: the original
  nudge-on-press made every hold start with a jump, and 0.5 screens/s was too slow.
- **Tap:** a press released within **250 ms** stops that axis and is topped up to exactly **1/8 of
  the screen** (screen space, so 1/(8 x level) of the desktop), with a quick ~90 ms glide.
- **Hold:** longer presses only pan (never the tap step) and glide out on release (~120 ms).
  Two keys held (e.g. Up + Right) pan diagonally; opposite keys cancel.
- **Pointer:** the view detaches from the pointer (the tracking path, #276). The pointer does not
  move while panning; on the next real mouse movement the pointer is placed in the view (the
  existing `warpPointer` takeover), a mouse button gives the view back without warping.
- **Edges:** the view centre is clamped so the view never leaves the monitor, and it respects the
  MPO wall exactly like mouse edge mode (`kMaxSafeTxMagnitude / level`) so keyboard panning can
  never reach the nearest-sampling TDR strip (#148, #242).
- **When it works:** zoomed (level > 1.001), not in Inspect, not while a game holds the mouse
  (`detector.locked()`), not over a shell input panel. Unlike caret tracking it does NOT require
  tracking to be on, and it is not blocked by a borderless fullscreen app (a pan key is an explicit
  request).
- **Magnify model:** Wind's level stays at 1x there, so the keys are never swallowed and Windows
  Magnifier receives Ctrl+Alt+arrows and pans natively. No extra code.
- **Caret tracking after a pan:** unchanged rules. The swallowed pan keys never move a caret, so the
  next real typing takes the view as today.

## 4. Design
- **Config** (`src/config.*`): `panLeftVk/panLeftMods`, `panRightVk/…`, `panUpVk/…`, `panDownVk/…`
  (defaults 37/3, 39/3, 38/3, 40/3 = Ctrl+Alt+arrows) and `panSpeed` (0.25-4, default 1.0).
  Sanitised with `CheckKeyBind` like the zoom keys; ini template documents them.
- **Pure motion** (`src/keyboard_pan.h`, new, tested): `KeyPan` holds per-axis velocity, the
  pending nudge distance and per-direction held time. `step(held[4], dtMs, level, monW, monH,
  speed) -> (dx, dy)` in desktop pixels; `active()` is true while a key is held or motion remains.
- **View owner** (`src/view_target.h`): new `ViewOwner::Keys`. Input `panning` (KeyPan active)
  makes Keys the owner; mouse movement takes the view back with `warpPointer` exactly as from
  Caret/Focus. The owner logic runs when tracking OR panning is possible (enabled rule in section 3).
- **Hook** (`src/input_router.*`): `setPanKeys(vk[4], mods[4])` and `setPanArmed(bool)`. Pan keys
  are tracked like every bound key (`isBoundKey`), but `keyBindMatches` counts a pan slot only
  while armed, so at 1x they pass through. The swallow decision stays once per press, so a press
  that began at 1x is never swallowed mid-way, and a swallowed press keeps its balanced UP.
- **Tick** (`src/main.cpp`): publishes `panArmed` (zoomed, not magnify, not Inspect, not locked);
  reads the four holds with the existing `comboHeld(vk, mods)`; steps KeyPan; when the owner is
  Keys, moves `t.viewCx/Cy` by the delta, clamps, and builds the frame with `DetachedMap`
  (the same path the caret owner uses). Hot-reload re-applies the pan keys.
- **Settings UI:** Keybinds section gains Pan left / right / up / down rows (key + modifiers,
  shared rules and refusal text) and a **Pan speed** slider. `droppedBinds` covers the pan slots.

## 5. Out of scope
Reading helpers, per-app behaviour, panning at 1x, rebinding in onboarding.

## 6. Testing and verification
- Unit: `KeyPan` (tap = exactly one nudge of screen/8 at any level, hold starts after 250 ms and
  reaches the set speed, release glides to rest, diagonals, opposite keys cancel, speed scales),
  `StepViewOwner` with `panning` (Keys owner, mouse takeover warps, button returns without warp),
  config defaults/sanitise, the shared rule fixture (Ctrl+Alt+arrows ok).
- UI (Playwright): the four rows show `Ctrl+Alt+Left` etc. by default, rebinding writes vk + mods,
  Pan speed slider writes `panSpeed`.
- Owner's PC (SendInput, signed build, only while the PC is idle): at 1x Ctrl+Alt+Left reaches a
  test window; zoomed, it is swallowed and the view moves (trackLog `view mouse -> keys` plus the
  view centre); a tap moves one step; a mouse move afterwards places the pointer in the view.

## 7. Delivery
Branch `feat/287-keyboard-pan`, one PR closing #287, version **0.15.0**, README controls,
CLAUDE.md (one line), docs/architecture 06-input and 07-cursor.
