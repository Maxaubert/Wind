# Config restart prompt + config/magnifier lifecycle coupling

Date: 2026-07-09
Status: approved (design)

Two independent features in the WindConfig UI and its relationship to the Wind
magnifier process.

## Context

`model` (render | transform) is read once at Wind launch and is deliberately not
hot-swappable (`config.h:65`). Every other key hot-reloads: Wind watches the ini
directory and re-reads on change (`main.cpp:200-220`). So changing `model` in the
config UI writes the ini, Wind reloads, ignores `model`, and resets the zoom
level - which reads to the user as "Apply soft-rebooted it but nothing changed."
The only way to actually switch model today is to quit and relaunch Wind by hand.

Separately, the config window currently outlives the magnifier: quitting Wind
(Ctrl+Alt+Q, tray Quit, or a crash) leaves an orphaned WindConfig window that is
configuring a process that no longer exists.

### Constraints discovered in the existing code

- **UIPI blocks `PostMessage` from WindConfig to Wind.** The deployed `Wind.exe`
  is UIAccess (higher integrity) than the normal-IL `WindConfig.exe`, so window
  messages are silently swallowed. The codebase already works around this with a
  named kernel event, `Local\Wind_QuitRequest` (`config_ui/main.cpp:155-162`,
  `main.cpp:1028`), which is not gated by UIPI. Any new signalling must use a
  kernel object, not a window message.
- **Wind already has a single-instance eviction handshake** (`main.cpp:801-817`).
  A newly launched instance tries the `Local\Wind_Magnifier_SingleInstance` mutex;
  if it is held, it signals `Local\Wind_QuitRequest` to the incumbent, waits up to
  3s for the mutex, and falls back to terminating the straggler. **This means a
  restart requires no new IPC: relaunching `Wind.exe` evicts the old instance.**
  WindConfig's own comment already notes relaunching "would kill+restart it".
- **`WindRunning()` conflates "absent" with "could not tell".** It returns `false`
  when `CreateToolhelp32Snapshot` fails (`config_ui/main.cpp:35`), not only when
  Wind is gone.
- **WindConfig can show its window while Wind is not running.** If launching
  `Wind.exe` fails at startup it shows onboarding anyway (`config_ui/main.cpp:246`).

## Goals

1. Changing `model` prompts for confirmation and, on confirm, actually restarts Wind.
2. The config window closes when the magnifier exits, by any means.

## Non-goals

- Making `model` hot-swappable. Restart-only is a deliberate, already-locked design decision.
- Changing what the Settings title-bar X does. It closes the config window; a separate
  button minimizes. Both stay as they are.
- The open pan-wobble defect and the caret latency. Tracked separately.

---

## Feature A: model change requires a restart

### Behaviour

Changing the `model` select to a value different from the saved one opens a modal:

> **Restart required**
> Changing the magnifier model requires restarting Wind.
> [ Cancel ]  [ Restart Wind ]

- **Restart Wind** - write `model` to the ini, then relaunch `Wind.exe`.
- **Cancel** - revert the select to its previous value. **Write nothing.**
- If Wind is not currently running, skip the modal entirely and just save. There
  is nothing to restart, and the next launch picks up the new value.

### Why Cancel must not write

Keeping the ini's `model` always equal to the *running* model removes a whole class
of bug. The alternative (write now, apply on next manual restart) creates a state
where the ini says `transform` while the live process is `render`. Everything
downstream then lies: the modal's "did the model change?" comparison, and the
UI's `showIf` gating that shows/hides transform-only and render-only rows.

### Mechanism

The modal lives in the web UI (Svelte), not a Win32 `MessageBox`, to match the
app's custom chrome.

On confirm the UI sends `setConfig{key:"model"}` followed by a new
`window{action:"restartWind"}` message. The native handler calls a `LaunchWind()`
helper - factored out of the existing startup launch at `config_ui/main.cpp:255-262`
- which does `ShellExecuteW` on `ExeDir()\Wind.exe`.

No new named event is required. The freshly launched Wind performs the existing
eviction handshake: it finds the mutex held, signals `Local\Wind_QuitRequest`,
waits for the incumbent to exit cleanly (restoring the cursor, resetting zoom,
removing the tray icon), then takes ownership and starts on the new `model`.

Ordering matters: the ini write must complete before the relaunch, because the new
instance reads `model` at startup.

### Interaction with Feature B

The relaunched instance appears in the process list *before* it signals the
incumbent to quit, so `WindRunning()` never observes a gap and Feature B's watchdog
will not fire mid-restart. This overlap is what makes the two features safe
together; the consecutive-miss requirement below is the belt to that braces.

### Error handling

If `ShellExecuteW` returns <= 32 the relaunch failed. Leave the incumbent Wind
running and untouched (it was never signalled - only a successful launch signals
it), and surface an error in the UI rather than failing silently.

---

## Feature B: config window closes when Wind exits

### Behaviour

When the magnifier process goes away - Ctrl+Alt+Q, tray Quit, or a crash - the
config window closes. "The config window should not exist if the magnifier is
offline."

### Mechanism

A `WM_TIMER` in WindConfig's `WndProc`, 1s period, calling the existing
`WindRunning()`. On confirmation that Wind is gone, `PostMessageW(g_hwnd, WM_CLOSE, 0, 0)`.

Chosen over the two alternatives:

- *Wind signals a `Local\Wind_Shutdown` event on exit.* Instant and cheap, but a
  crash never sets the event - the config window would survive exactly the case
  where an orphan is most confusing.
- *Wait on Wind's process handle.* Instant and crash-proof, but requires
  `OpenProcess` from a lower-IL process against a UIAccess one. It likely works,
  but the codebase already documents being burned by a cross-IL handle assumption
  here (`config_ui/main.cpp:28-31`), so the feature should not rest on it.

A <=1s latency is invisible for "the window went away when I quit the app", and a
poll cannot be defeated by a crash or a privilege boundary.

### Two guards, both load-bearing

1. **Require 2 consecutive misses before closing.** `WindRunning()` returns `false`
   on `CreateToolhelp32Snapshot` failure, not just on absence. A single transient
   failure must not close the user's config window.
2. **Arm only after Wind has been observed running at least once.** WindConfig can
   legitimately display a window while Wind is down: if `Wind.exe` fails to launch
   at startup it shows onboarding anyway. An unconditionally-armed watchdog would
   close that window immediately, hiding the very error the user needs to see.

### Decision helper (unit-testable)

The guard logic is pure and gets a doctest, matching how the repo tests its other
pure logic (`transform.cpp`, `zoom_controller.cpp`):

```cpp
// Returns true when the config window should close.
// running: this poll's WindRunning() result.
// armed:   set once running has ever been observed true.
// misses:  consecutive false observations; caller-owned, reset on true.
bool ShouldCloseOnWindGone(bool running, bool& armed, int& misses);
```

Cases: never-armed + not running -> false (never closes). Armed + one miss ->
false. Armed + two consecutive misses -> true. A `true` between misses resets the
counter.

---

## Testing

Unit (doctest): `ShouldCloseOnWindGone` across the four cases above.

Manual verification:

1. Switch model, Cancel -> dropdown reverts; ini `model` unchanged; Wind untouched.
2. Switch model, Restart Wind -> Wind restarts and comes up on the new model
   (verify by the transform-only rows appearing, and by observed behaviour);
   ini `model` matches the running model; tray icon returns exactly once.
3. Quit Wind with Ctrl+Alt+Q while config is open -> config closes within ~1s.
4. Kill `Wind.exe` from Task Manager -> config closes (crash path).
5. Launch WindConfig with Wind not running -> Wind launches, config stays open.
6. Change a hot-reloadable key (e.g. `outlineThickness`) -> no modal, applies live.

## Issue / PR mapping

- **Feature A** belongs to the model selector, issue #126, on `feat/transform-model`.
- **Feature B** is independent config-app lifecycle. It gets its own issue and PR,
  landed after #126 to avoid tangling the branches.
