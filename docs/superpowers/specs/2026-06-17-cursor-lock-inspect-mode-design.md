# Inspect mode (cursor-lock) — design

Date: 2026-06-17
Status: approved (brainstorm), pending implementation plan
Related: render engine (`docs/superpowers/specs/2026-05-25-own-renderer-design.md`),
auto cursor sensitivity (`docs/superpowers/specs/2026-05-26-auto-cursor-sensitivity-design.md`),
`LockDetector` (game-lock heuristic, `src/lock_detector.*`).

## Problem

Two recurring annoyances while using a magnifier, both rooted in the OS cursor sitting on
whatever you are trying to look at:

1. **Hover that fires on its own.** Resting the cursor on a video thumbnail starts an
   autoplay preview; resting on a media element triggers a hover animation. You wanted to
   look, not activate.
2. **Hover you want to keep.** A tooltip appears while the cursor is over a label. To read
   it you move toward it, leave the label, and the tooltip vanishes.

Both want the same primitive: **decouple "where the lens looks" from "where the OS cursor
is."** A non-clickthrough hover-suppression overlay was considered and rejected: it would
solve case 1 but actively breaks case 2 (it prevents the tooltip from ever appearing).

## Solution overview

**Inspect mode** ("cursor lock" in code). A toggle, bound to a key, active only while zoomed.

- **Toggle ON:** the real OS cursor freezes where it is (any active hover stays alive).
  Panning continues to move the **lens** from hand motion. A reticle (the normal drawn
  cursor) sits at lens center with a small lock indicator beside it.
- **Toggle OFF (no click):** the real cursor warps to the lens-center reticle and normal
  follow resumes from there. The frozen hover point is abandoned.
- **Click while locked ("click to commit"):** the real cursor snaps to the reticle, the
  click lands there, and lock turns OFF in one motion. This is the always-available escape
  hatch so you can never get stranded in lock mode.

The frozen hover point is "live" only for the duration of the lock.

## Behavior detail

### State machine (pure, testable)

New pure-logic unit `src/cursor_lock.{h,cpp}` — `CursorLockController` — following the
existing pure pattern (`ZoomController`, `LockDetector`, `transform`): NO `<windows.h>`, so it
compiles into the desktop-free `WIND_TESTS` build.

State: `locked_` (bool, default OFF).

Per-tick / per-event inputs from `main.cpp`:
- `togglePressed` (rising edge of the bound key)
- `clickPressed` (left/right button-down observed while locked)
- `zoomedIn` (level > 1.0)
- `recenter`, `monitorRetarget` (reset signals)

Outputs the tick loop consumes:
- `locked()` — am I locked.
- `panFromRaw()` — pan source this tick: raw mickeys while locked, OS-cursor delta while free.
- `suppressSetCursor()` — while locked, the tick must NOT `SetCursorPos` / update
  `lastSetVirtual`, leaving the real cursor frozen.
- `consumeWarpToReticle()` — one-shot, true on any lock→free transition (toggle-off or
  click): the tick warps the OS cursor to the current lens-center desktop point and resyncs
  `lastSetVirtual` before resuming follow.

Transitions:
- **Toggle while zoomed:** flip lock.
  - free→locked: freeze (begin suppressing `SetCursorPos`, switch pan to raw).
  - locked→free: emit warp-to-reticle, resume follow.
- **Click while locked:** emit warp-to-reticle, force lock OFF (the click itself is fired by
  the hook, see below).
- **Auto-clear** on zoom-out, `recenter`, and `monitorRetarget` — the same reset points
  `LockDetector.reset()` is already called at. No warp on auto-clear (zoom-out tears down the
  overlay; recenter/retarget re-baseline the cursor anyway).
- Toggle is **ignored when not zoomed**.

### Panning & cursor wiring (`main.cpp`)

Reuses existing paths:
- **Locked pan** = the existing game-locked branch (`main.cpp:376`): `dx/dy` from
  `rawDx/rawDy * cursorSensitivity`. Lens moves with the hand; the OS cursor does not.
- **Frozen cursor implementation:** the freeze is a 1px `ClipCursor` rect centered on the
  freeze point (truly stationary -- no `WM_MOUSEMOVE` fires, no pointer ballistics, the OS
  cursor cannot drift). This is distinct from merely skipping `SetCursorPos`: a skipped
  `SetCursorPos` still allows the cursor to wander if anything else moves it; the 1px clip
  enforces the freeze at the OS level.
- **Locked = skip the `SetCursorPos` + `lastSetVirtual` update** (`main.cpp:422-425`). The
  real cursor stays frozen at the frozen point.
- The reticle is the drawn cursor at lens center — already where it is drawn today
  (centered-cursor design), so no positioning change, only the lock indicator.
- While `locked()`, **bypass `LockDetector`** entirely: manual lock supersedes the game-lock
  heuristic. Both regimes integrate a delta into the same accumulator, so toggling lock never
  snaps the lens position (same anti-flicker invariant as the free/game-locked switch).

### Click-to-commit (the one genuinely tricky part)

Today clicks land correctly only because the OS cursor is continuously `SetCursorPos`'d under
the reticle. While locked it is frozen elsewhere, so a raw click would land on the frozen
point, not where you are aiming.

Fix: the existing `WH_MOUSE_LL` hook (`input_router.cpp`) must, on left/right button-**down**
*while locked*, **synchronously `SetCursorPos`(lens-center desktop point) in the hook callback
before the event propagates**, then signal the tick to clear lock. The down event then lands
at the reticle. The click is **NOT swallowed** (it must reach the underlying app), and the
matching up event follows because the OS cursor was physically moved there.

Shared state: the tick publishes the current lens-center desktop point each frame to an
`std::atomic<POINT>`-equivalent (two atomics, or a packed 64-bit) that the hook thread reads.
This is the highest-risk piece and the primary manual-verification target (timing of the warp
vs. the button dispatch).

### Lock indicator (rendering)

Add one field to `RenderFrameParams` (e.g. `bool cursorLocked`). When set, the cursor-draw
path in `render_engine.cpp` draws the normal arrow plus a small ring / lock glyph beside it.
Modest addition; reverts to the plain arrow on unlock. If the drawn cursor is hidden
(`cursorMode == 2`), no reticle/indicator is drawn but lock still functions (pan blind, click
commits).

## Config & UI

- One new keybind in `config.h`: `cursorLockVk` / `cursorLockMods` (VK + modifier mask, same
  bit layout as the zoom combos). **Unbound by default (`0`)** — binding it is the opt-in, so
  no separate master enable flag (YAGNI). The bind is **VK-only (no modifier support)** to keep
  the keyboard-hook swallow simple and correct: the hook matches on VK alone, and modifier-combo
  swallows risk eating modifier keys in other apps. This matches `recenterVk` behavior.
- `config.cpp`: parse + sanitize through `IsForbiddenBindVk` (left/right click, Backspace,
  Win keys can never be bound). Round-trips in the ini like `recenterVk`.
- Keyboard-hook swallow: add `cursorLockVk` to `g_input.setKeys(...)` and the re-bind check in
  the hot-reload path (`main.cpp:~238-243`), so the toggle key is eaten and never double-fires
  into the focused app. Balanced down/up swallow like every other bound key.
- UI: one keybind row in the Svelte settings (`ui/src/settings-schema.js`, surfaced in
  `Settings.svelte`), live-applied like the other keybind rows. Label: **"Inspect mode"**
  (description: freeze the cursor to keep a hover/tooltip alive while you pan and read).

## Edge cases

- **Multi-monitor:** lock auto-clears on retarget; cannot be locked across a monitor switch
  (consistent with the "one monitor while zoomed" rule).
- **Click-to-commit inside a clip-locked game:** the game's `ClipCursor` may fight the warp;
  documented limitation, same family as the existing raw-input-game caveat in CLAUDE.md.
- **Hide-cursor mode:** reticle/indicator not drawn, lock still works (blind pan + commit).
- **Forbidden keys / safety:** enforced in the same three places as other binds (hook never
  swallows them, `ParseConfig` sanitizes, config UI capture refuses).

## Testing

- **Pure unit tests** `tests/test_cursor_lock.cpp` (doctest) for `CursorLockController`:
  toggle / click / zoom-out / recenter / retarget sequences → expected `locked()`,
  `panFromRaw()`, `suppressSetCursor()`, and one-shot `consumeWarpToReticle()`. No
  `<windows.h>`; added to the `WIND_TESTS` source list.
- **Manual verification:** `WIND_SELFTEST=1` for the indicator render; live testing for the
  click-to-commit warp timing (real thumbnail autoplay + a real tooltip), and that the toggle
  key is swallowed from the focused app.

## Out of scope (YAGNI)

- The rejected hover-suppression overlay.
- A separate "frozen point" on-screen marker (chose the subtle arrow+ring indicator instead).
- A hold-to-lock variant or a master enable flag — the single unbound-by-default toggle is the
  whole opt-in surface.
