# 06. The input pipeline

Wind never has keyboard focus, yet it must see every bind press and every mouse movement
system-wide without breaking input for other programs. This chapter covers the input channels, the
hook thread, the swallowing rules, the bind safety rules and the recovery from silently evicted
hooks. The code is `src/input_router.*`, the `WM_INPUT` handling in `src/main.cpp`,
`src/keybind_rules.h` and `src/mouse_ballistics.*`.

## Channels

| Channel | Used for | Why this channel |
|---|---|---|
| `WH_MOUSE_LL` (`MouseProc`) | Mouse button and click binds, wheel binds, Inspect click interception | The only user-mode way to swallow a mouse event |
| `WH_KEYBOARD_LL` (`KbProc`) | Keyboard binds: down-state and swallowing | Same for keys; also the authority for bound-key state, since a swallowed key never reaches `GetAsyncKeyState` |
| Raw Input (`RIDEV_INPUTSINK`, `WM_INPUT` in `WndProc`) | HID mouse deltas for locked and Inspect panning; UP-event safety nets | HID level: unaffected by `ClipCursor`, `SetCursorPos` and `ShowCursor`, and not subject to the hook timeout |
| `RegisterHotKey` | Hide pointer, hotkey-mode quick zoom, Ctrl+Alt+Q quit | The OS suppresses a registered hotkey from other apps; these are toggles, not held keys |

**The view must keep moving when a game locks the cursor.** A mouselook game clips or recentres
the pointer every frame, so `GetCursorPos` deltas read zero. HID mickeys keep arriving, so
`LockDetector` uses the raw stream both to detect the lock and to pan through it. See
[07](07-cursor.md).

## The hook thread

Both LL hooks are installed by `HookThreadProc`, a thread that only pumps messages.

- **Windows services an LL hook on the thread that installed it**, holding each input event until
  that thread responds. On the main thread the hook waited behind the present, delaying all system
  mouse input by a frame.
- **A callback that misses `LowLevelHooksTimeout` gets the hook evicted**, so the thread runs at
  `THREAD_PRIORITY_TIME_CRITICAL`. That is safe because it does no other work.
- Callbacks touch only atomics on `InputState`/`InputRouter`: no I/O, no allocation. Hooks and
  `WM_INPUT` write; the tick drains (`drainRaw`, `drainCooked`, `keyPressed`, the held flags).

One mouse movement reaches both paths: the hook (latency-critical, stateless) and `WM_INPUT`
(accumulators for the tick). In a free desktop session neither pans the view; the tick's
`GetCursorPos` oracle does. The raw stream matters when the oracle is unusable (game lock, Inspect).

## Swallowing

Bound inputs are eaten so they never also fire in the focused app.

- **Balanced down/up.** A DOWN of a bound input is swallowed and recorded (`g_swallowedDown`,
  `g_kbSwallowedDown`); an UP is swallowed only if its DOWN was. Swallowing an UP whose DOWN the
  system saw leaves the input held system-wide (the stuck side-button bug, issue #113).
- **No stranded keys.** Records are cleared on every remap (`setButtons`, `setKeys`), and teardown
  runs `ReleaseSwallowedButtons`/`ReleaseSwallowedKeys`, which synthesize the missing UP.
- **Decide once per press.** A key bind is swallowed only when a bind on that key has all its
  modifiers held (`keyBindMatches`). A VK-only test once ate a plain F1 system-wide for a Ctrl+F1
  bind.
- **Mask key with Alt or Win.** Swallowing a key while Alt or Win is held injects one mask key (VK
  0xE8); otherwise Windows sees the modifier tapped alone and opens Start or the app's menu bar.
- **Wind's own injections** carry `kWindInjectTag` in `dwExtraInfo` and are skipped by the bind
  matcher. Other injectors (AutoHotkey) count as real input.
- Hide pointer and hotkey-mode quick zoom are suppressed by `RegisterHotKey`, not the hook.

## Bind rules

**One rule set for every bind:** `src/keybind_rules.h` (`CheckKeyBind`, `CheckWheelBind`,
`CheckClickBind`), mirrored in `ui/src/lib/keybindRules.js`. Both are tested against
`tests/fixtures/keybind_cases.txt`, so change both or the tests fail.

- `ParseConfig` reads an unsafe bind as unbound, the UI refuses it with the reason, and the hook
  never swallows an `IsForbiddenBindVk` key (left/right click, Backspace, the Windows keys).
- Refused: typing keys alone; Shift or AltGr plus a typing key (AltGr sends Ctrl+Alt, so
  Ctrl+Alt + a typing key is refused); combos Windows reserves (Alt+F4, Win+L and similar).
- Button codes: 1/2 side buttons, 3/4/5 left/right/middle click. Clicks need modifiers, never Ctrl
  or Shift alone.
- The most specific matching slot wins.
- `panKeysOn`, `hideCursorOn` and `cursorLockOn` (default 1, per profile) make `ParseConfig` read
  that bind as unbound when 0, while the ini keeps the binding.

## Wheel and pan binds

**Wheel zoom.** Button codes 6 and 7 are wheel up and down in the zoom slots: one zoom step per
notch, swallowed only while the slot's modifiers are held, so plain scrolling is untouched. The
wheel may use Ctrl alone, because the notch is swallowed. A notch zooms as far as holding the bind
for 0.1 s. The old `zoomWheelMods` key is migrated into free slots once at start
(`MigrateIniFiles`) and stays honoured only when a direction has no free slot.

**Keyboard panning** (`panLeftVk` .. `panDownVk` + mods, unbound by default). Pan slots match only
while the tick arms them (`setPanArmed`: zoomed, not Inspect, no mouselook lock), so at 1x the keys
reach apps. The swallow is decided once per press, and the tick pans only on presses the hook
swallowed (`keySwallowed`), as the `ViewOwner::Keys` detached view (`src/keyboard_pan.h`).

## Raw Input as the safety net

Both hooks have a Raw Input backstop for lost UP events, in the `WM_INPUT` handler:

- **Keyboard:** `RI_KEY_BREAK` calls `rawKeyUp`. Raw Input still delivers the UP an evicted hook
  missed, which would otherwise leave a zoom bind held forever.
- **Mouse:** button UPs call `rawButtonUp`. UP only, so the net can clear held state but never set
  it.
- **Reordering guard.** `WM_INPUT` drains up to a tick late, so a raw UP can arrive after the hook
  recorded the next press. The net skips the clear when the hook recorded a DOWN for that input
  AFTER the UP, comparing event times (`GetMessageTime()` of the `WM_INPUT` against the hook
  struct's `.time`, `src/event_order.h`), not the wall clock, so a main-thread stall of any length
  cannot make a stale UP cancel a live hold.
- DOWN edges stay hook-authoritative while the hook is active; `WM_INPUT` writes down-state only in
  the no-hook fallback.

## Eviction and the watchdog

Windows evicts an LL hook whose callback misses `LowLevelHooksTimeout` without any error; the
handle stays valid and the callback never fires again. A game's launch load spike can trigger it:
keyboard binds die while mouse binds survive (issue #156).

**The tell costs nothing:** a live hook swallows every bound key, so `GetAsyncKeyState` seeing a
bound key held while `keyPressed()` says up means the hook is gone. After a 250 ms dwell
(`kKbHookDeadMs`), `requestKbHookReinstall` drops the authority claim (the next tick polls),
releases swallowed-key records and posts `kMsgSetKbHook` to the hook thread, which must install
the hook itself.

**`noSwallowApps`.** An LL keyboard hook taxes system input just by existing: the raw input thread
waits on the hooking thread for every keystroke, so a held auto-repeating key puts a stall into the
mouse stream of the foreground game. Listing an exe uninstalls the keyboard hook while that app is
foreground (a ~10 Hz probe, `setKeyboardHookWanted(false)`); binds keep working through polling.
Off by default.

## Bound keys still reach games

**LL hooks cannot block Raw Input**, and most games read Raw Input, so a bound key or button still
reaches a raw-input game whatever the hook returns. There is no user-mode API to suppress raw input
to another process. The only fix is a kernel filter driver, which Wind does not use (no-driver
design, anti-cheat ban risk). Swallowing works in desktop apps and browsers. Guidance for users:
bind keys the game does not use. Game-Inspect sidesteps this for Inspect mode only, see
[07](07-cursor.md).

## Inspect: click routing and ballistics

While Inspect is on, the real cursor is frozen by a 1 px `ClipCursor`.

**Click-to-look-point.** `MouseProc` swallows real left/right presses while `inspectActive` is set,
records the DOWN per button (`g_commitDown`) and counts presses (`commitLeft`/`commitRight`), so a
fast double-click is not lost. The tick fires an absolute click at the look point per press. The
injected click carries `LLMHF_INJECTED` (the hook skips it) and its absolute move is ignored by the
raw accumulator, so the look point does not move. In game-inspect the presses are discarded.

**Ballistics** (`src/mouse_ballistics.*`, pure). The frozen cursor makes the normal pan oracle read
zero, so the look point pans from raw mickeys run through Windows' pointer ballistics per
`WM_INPUT` packet: the pointer-speed multiplier plus, with "Enhance pointer precision", the
SmoothMouse curve, normalized so slow movement is 1:1 with the slider. The curve is blended at
`accelStrength` (default 0.3) because `WM_INPUT` can coalesce HID reports and over-accelerate.
`cookPacket` runs only while `inspectActive`; the tick drains with a sub-pixel carry.
