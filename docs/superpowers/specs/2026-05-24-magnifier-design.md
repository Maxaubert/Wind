# Fullscreen Magnifier - Design Spec

- **Date:** 2026-05-24
- **Status:** Approved (brainstorming complete; pending written-spec review)
- **Working directory:** `Wind` (product name TBD; referred to here as "the magnifier")

## 1. Summary

A lightweight, standalone **fullscreen magnifier** for Windows that replaces the
built-in `Magnify.exe`. It provides smooth, continuously-variable zoom with minimal
performance cost, and - the defining feature - **the magnified view keeps tracking
mouse movement even when a game hides, clips, or center-locks the OS cursor**.

It is intended as a better everyday magnifier that is *also* genuinely usable while
gaming, where the Windows magnifier is heavy and loses cursor tracking.

## 2. Goals

1. **Fullscreen magnification** of the whole display.
2. **Responsive** - reacts immediately to input.
3. **Light** - negligible CPU/GPU impact; built with gaming in mind.
4. **Smooth, gradual zoom** - continuously variable, not the 25%/100% steps of the
   Windows magnifier.
5. **Works in games** - must not be defeated by `ShowCursor`, `ClipCursor`,
   `SetCursorPos`, or raw-input center-locking. **The lens must remain movable even
   when the game hides/clips/locks the cursor.** This is the primary differentiator
   and the core problem being solved.

## 3. Non-goals (v1)

- **Exclusive-fullscreen games.** v1 magnifies the desktop, normal apps, and games in
  **borderless / windowed-fullscreen** mode (the modern default). True exclusive
  fullscreen bypasses the desktop compositor and is out of scope. Documented as a
  future "Mode 2" (capture-based engine).
- DLL injection into games (the Magnifier-In-Games approach) - kept as a documented
  optional future fallback, not built in v1.
- Color filters, lens shapes/effects, and a full GUI settings window.

## 4. Prior art (in sibling repos, reused as knowledge only)

- **`Magnifier-In-Games`** - C + MinHook DLL that injects into a game and neuters
  `ShowCursor`/`SetCursor`/`ClipCursor`/`SetCursorPos` so the *Windows* magnifier can
  follow the OS cursor. We solve the same problem differently (Raw Input, no
  injection), because we own the magnifier this time. Its build setup (`cl.exe` via
  `vswhere` in `build.bat`) is the template for ours.
- **`HoldToZoom`** - AHK controller for the Windows magnifier's native zoom on mouse
  side buttons, with a robust triple-mechanism modifier-release. We reuse its
  control ergonomics (forward/back buttons, hold-to-repeat) and its
  release-safety pattern (explicit release + physical-state watchdog + on-exit reset).

## 5. Technical approach

### 5.1 Zoom engine - Windows Magnification API

Use the fullscreen transform of the Magnification API:
`MagInitialize()` -> `MagSetFullscreenTransform(level, xOffset, yOffset)` ->
`MagUninitialize()`.

- It runs on the **DWM GPU compositor** - the same path `Magnify.exe`'s full-screen
  mode uses - so there is **no per-frame screen copy**; impact is negligible.
- `level` is a **float**, so calling it every tick with an interpolated value yields
  genuinely **smooth, continuous zoom** (goal #4).
- It magnifies everything composited by DWM: desktop, apps, and borderless games. It
  is **not** affected by the game's cursor calls (goal #5, visual half).
- `MagSetInputTransform` is **not** used (it needs UIAccess / a signed binary in
  Program Files). We do a purely visual transform, so we avoid that deployment burden.

**Offset math.** `xOffset/yOffset` is the top-left, in unmagnified screen pixels, of
the source region being magnified. The visible region is `screenW/level` x
`screenH/level`. To center the view on the virtual lens center `C`:

```
viewW = screenW / level;  viewH = screenH / level
xOffset = clamp(C.x - viewW/2, 0, screenW - viewW)
yOffset = clamp(C.y - viewH/2, 0, screenH - viewH)
```

### 5.2 Cursor tracking - Raw Input, no injection (the core feature)

The lens always follows the cursor. To survive cursor lock/hide/clip without
injecting into the game, **our own process** reads mouse movement at the HID level:

- Register Raw Input for the mouse with `RIDEV_INPUTSINK` (so `WM_INPUT` arrives even
  when the game is foreground). Raw deltas (`lLastX`/`lLastY`, `MOUSE_MOVE_RELATIVE`)
  are reported independently of `ShowCursor`, `ClipCursor`, and `SetCursorPos` - the
  same signal the game itself reads to aim.
- **Auto-blend tracker:**
  - **Free mode** (desktop, windowed apps): the OS cursor moves normally; set the
    virtual center to `GetCursorPos()`.
  - **Locked mode** (game has frozen/clipped/centered the cursor): detected when
    `GetCursorPos()` stops changing while raw deltas keep arriving. Integrate raw
    deltas (scaled by a configurable sensitivity) into the virtual center.
  - On return to free movement, resync the virtual center to `GetCursorPos()`.
- The virtual center is always clamped to screen bounds. A **recenter** binding snaps
  it back to screen center.

This keeps the lens movable in cursor-locked games (goal #5, control half) with **no
code running inside the game process** - lighter and anti-cheat-safe.

### 5.3 Zoom control - continuous hold-to-zoom on mouse side buttons

- **Forward button (XButton2, default): hold to ramp zoom IN.**
- **Back button (XButton1, default): hold to ramp zoom OUT.**
- **Release freezes the level** wherever it stopped (the back button is how you return
  toward 1.0x). Bindings are configurable.
- While a button is held, `level` ramps **multiplicatively** (perceptually-linear:
  `level *= zoomFactor^dt`), clamped to `[1.0, maxLevel]`. Multiplicative ramping makes
  the zoom feel even across the whole range.
- **Release safety (from HoldToZoom):** (1) explicit stop on button-up, (2) a
  per-tick **physical-state watchdog** (`GetAsyncKeyState`/physical key state) that
  stops ramping if the button is no longer physically down even when an up event was
  missed, and (3) an on-exit handler that resets the transform. A dropped button-up
  can never strand the screen mid-zoom.

## 6. Module breakdown (one concern each; pure logic isolated for testing)

- **`ZoomController`** *(pure logic, unit-tested)* - hold-to-zoom state machine.
  Input: button down/up + elapsed time + config. Output: current `level`. No I/O.
- **`Tracker`** *(pure decision logic + thin Raw Input I/O)* - virtual lens center.
  The blend / lock-detection / delta-integration / clamp math is pure and tested; the
  only I/O is `WM_INPUT` registration and `GetCursorPos`.
- **`Transform`** *(pure logic, unit-tested)* - converts `(center, level, screenRect)`
  into clamped `(xOffset, yOffset)` per the offset math above.
- **`MagnifierEngine`** *(I/O)* - wraps `MagInitialize` /
  `MagSetFullscreenTransform` / `MagUninitialize`.
- **`InputRouter`** *(I/O)* - registers Raw Input and a low-level keyboard/mouse hook;
  routes button/key events to `ZoomController` and raw deltas to `Tracker`.
- **`Config`** - loads/saves an INI file next to the exe; hot-reloads on change.
- **`TrayApp` / `main`** - single-instance mutex, tray icon (enable/disable, edit
  config, quit), message loop, and the update tick that wires it all together.

## 7. Data flow / update loop

```
WM_INPUT (raw deltas) ---------------> Tracker.update()
hook (button/key events) -----------> ZoomController.update() / recenter

every tick (display refresh, capped ~144 Hz):
    level   = ZoomController.level()
    center  = Tracker.center()
    offsets = Transform(center, level, screenRect)
    MagnifierEngine.setTransform(level, offsets)
```

Each tick is a couple of cheap compositor calls. Input updates shared state
asynchronously; the tick just reads it.

## 8. Error handling & lifecycle

- Declare **Per-Monitor-V2 DPI awareness** (manifest) so offset pixel math is correct
  on scaled displays.
- If `MagInitialize` fails, show a tray balloon explaining why and exit cleanly.
- On quit / crash / exit, reset to `MagSetFullscreenTransform(1.0, 0, 0)` then
  `MagUninitialize` - never leave the screen zoomed.
- **Single instance** via a named mutex.
- **Multi-monitor:** v1 magnifies the monitor under the cursor. Exact
  fullscreen-transform behavior across monitors is verified during implementation; if
  the API only transforms the primary monitor, that limitation is documented for v1.

## 9. Testing / verification loop (day one)

- **Unit tests** (single-header C++ harness, e.g. doctest) for the pure modules:
  - `ZoomController`: ramp reaches `maxLevel` on sustained hold, ramps down to exactly
    1.0, freezes on release, watchdog stops on missed button-up, multiplicative curve
    is monotonic.
  - `Tracker`: free->locked->free transitions, delta integration + sensitivity,
    clamping, recenter.
  - `Transform`: centering and edge clamping at several levels and screen sizes.
- **Build gate:** compiles clean at `/W4`.
- **Manual smoke checklist:** desktop zoom in/out feel; a borderless game with the
  cursor hidden/locked (confirm the lens still pans and zooms); Task Manager check
  that CPU/GPU cost is negligible while idle-zoomed and while panning.

## 10. Defaults (configurable via INI)

| Setting | Default |
|---|---|
| Zoom-in binding | Mouse **XButton2** (forward), held |
| Zoom-out binding | Mouse **XButton1** (back), held |
| Recenter binding | Configurable key (unset by default) |
| Max zoom | **8.0x** |
| Zoom speed | reaches 1.0x -> 8.0x in ~1.2 s of continuous hold (multiplicative) |
| Raw-input pan sensitivity | 1.0 (scales locked-mode panning) |
| Tick rate | display refresh, capped ~144 Hz |
| Config file | `magnifier.ini` next to the exe, hot-reloaded |
| Enabled on launch | yes; runs from the tray |

## 11. Build & toolchain

- **C++**, built with `cl.exe` located via `vswhere`, driven by `build.bat` (mirrors
  `Magnifier-In-Games`). Links `Magnification.lib` and `user32.lib`.
- App manifest sets Per-Monitor-V2 DPI awareness.
- No external runtime dependencies; single small `.exe`.

## 12. Future modes (explicitly deferred)

- **Mode 2 - exclusive fullscreen:** Desktop Duplication (`IDXGIOutputDuplication`) +
  a Direct3D fullscreen overlay, for games that refuse borderless. Heavier; opt-in.
- **MIG injection fallback:** bundle/port the existing DLL-hook approach for
  pathological games where Raw Input tracking is insufficient.
- Color filters / lens shapes, GUI settings window.
