# Silent model restart + swap-model hotkey

Date: 2026-07-10
Status: approved (design)

Two related changes to how the magnifier `model` (render | transform) is switched.

## Context

`model` is read once at Wind launch and is deliberately not hot-swappable
(`config.h:66`); the two values select entirely separate engines (DXGI capture +
D3D11 overlay vs `MagSetFullscreenTransform`). Every other config key hot-reloads
(the core dir-watches the ini). Switching `model` therefore requires relaunching
`Wind.exe`.

The previous design (`2026-07-09-config-restart-and-lifecycle-design.md`, Feature A)
gated a model change in WindConfig behind a "Restart required" confirmation modal.
In practice a deliberate Apply of a model change and a hot-reload look identical to
the user, so the confirm step is friction with no information value. And there is
today no way to switch model without opening the settings UI.

The existing restart mechanism needs no new IPC: relaunching `Wind.exe` while an
instance is running triggers the single-instance eviction handshake
(`main.cpp:820-832`) - the new instance signals `Local\Wind_QuitRequest` to the
incumbent, waits for it to exit cleanly (cursor restore, tray removal, Inspect clip
release), then takes ownership. WindConfig already relies on this via its
`restartWind` handler (`config_ui/main.cpp:175-181`).

The INI is edited key-by-key with the pure helpers in `src/config_ui/ini_edit.h`
(`ReadIniValues`, `UpdateIniText`) - no `<windows.h>`, so they are reusable from the
core as well as WindConfig.

## Goals

1. Applying a model change in WindConfig restarts Wind silently - no confirm modal.
2. An optional, unbound-by-default hotkey alternates `render` <-> `transform` from
   anywhere, restarting Wind onto the flipped model.

## Non-goals

- Making `model` hot-swappable in-process. Restart-only remains a locked design
  decision; both features restart.
- Changing Feature B (config-window-closes-when-Wind-exits) from the prior spec.

---

## Feature A: model row applies + restarts silently

### Behaviour

Changing the `model` select and pressing **Apply** writes the new `model` (plus any
other pending edits) to the ini and relaunches Wind immediately. No modal.

The relaunch-failure path is preserved: if `Wind.exe` cannot be launched, the UI
shows a "Couldn't restart Wind" box AND reverts so the ini's `model` again equals the
running process's model.

### Mechanism (`ui/src/Settings.svelte`)

- `apply()`: when `String(values.model) !== String(saved.model)`, call `commit()`
  then `windowControl('restartWind')`. Otherwise just `commit()`.
- Remove the confirm branch: `confirmRestart`, `cancelRestart`, and the
  "Restart required" modal markup.
- Keep the `restartFailed` message handler and its error box. On `restartFailed`:
  set `values.model` back to `saved.model` and `setConfig('model', saved.model)` to
  rewrite the ini, then show the box.

### Why revert on failure

Keeping the ini's `model` equal to the *running* model is load-bearing: the schema's
`showIf` gating shows/hides transform-only and render-only rows off the ini value. A
failed relaunch that left `model=transform` in the ini while the live process runs
`render` would reveal rows for an engine that is not loaded. On success, ini == new
== running, so the invariant holds without a confirm step.

The native `restartWind` handler (`config_ui/main.cpp`) is unchanged - it already
`ShellExecute`s `Wind.exe` and posts `restartFailed` on `<= 32`.

---

## Feature B: optional swap-model hotkey

### Behaviour

A new keybind, unbound by default. When bound and pressed - zoomed or idle - Wind
flips `model` to the other value, persists it, and restarts onto the flipped model.
Pressing again flips back. The restart is the same brief teardown as Feature A
(active zoom ends, cursor restores, tray icon re-adds over ~0.5-1s); this was
confirmed acceptable.

### Config (`config.h` / `config.cpp`)

- New field `int swapModelVk = 0;` (0 = unbound). VK-only, no modifiers, mirroring
  `cursorLockVk`.
- Parse `swapModelVk` in `ParseConfig`; run it through `sanitizeVk` (so
  `IsForbiddenBindVk` keys - clicks, Backspace, Win - can never be bound).
- Add `swapModelVk=0` with a comment to the default-ini template in `LoadConfig`.

### Core (`input_router` / `main.cpp`)

- `swapModelVk` joins the LL-keyboard-hook key set via `g_input.setKeys(...)` so it
  is swallowed (never double-fires into the focused app) and is authoritative for
  down-state, exactly like `recenterVk` / `cursorLockVk`. `setKeys` gains a parameter;
  its two call sites (startup `main.cpp:920`, hot-reload `main.cpp:246`) pass the new
  vk, and the hot-reload guard compares `swapModelVk` too. Because the *binding* is
  hot-reloadable, changing the key in settings re-arms the hook live; only *pressing*
  it restarts.
- In `RunTick`, rising-edge detect `swapModelVk` (a `swapKeyWasDown` flag beside
  `recenterKeyWasDown`), processed unconditionally so it works whether or not zoomed.
  On the rising edge:
  1. `flipped = wind::FlipModel(t.cfg.model)`.
  2. Read the ini (`ResolveIniPath()`), `wind::UpdateIniText(text, "model", flipped)`,
     write it back atomically.
  3. `ShellExecuteW(nullptr, L"open", <ExeDir>\Wind.exe, ...)`. The new instance's
     eviction handshake stops this one. No new IPC.
  4. If `ShellExecuteW` returns `<= 32`: do not quit; rewrite the ini's `model` back
     to `t.cfg.model` and log the failure (the core has no UI to prompt).
- `ini_edit.cpp` is added to the `Wind.exe` compile set (it is pure; currently only
  linked into `WindConfig.exe`). The core gains a small read-file + atomic-write
  next to its existing `config.cpp` file I/O, or reuses a shared helper.

### Pure helper (`config.cpp`, `WIND_TESTS`-visible)

```cpp
// render <-> transform. Any non-"transform" input maps to "render" (matches the
// ParseConfig fallback), so an unknown/corrupt value flips to a known-good one.
std::string FlipModel(const std::string& model);  // "transform" -> "render"; anything else -> "transform"
```

Precisely: `FlipModel("render") == "transform"`, `FlipModel("transform") == "render"`,
and any other value returns `"transform"` (so a corrupt `model` still flips to a valid
engine rather than sticking).

### Config UI (`ui/src/settings-schema.js` / `Settings.svelte`)

- One `keybind` row in the **Display** section, immediately under the `model` select:
  ```js
  { key:'__swapModel', type:'keybind', label:'Swap model hotkey',
    desc:'Press to switch between Render and Transform (restarts Wind). Right-click to clear.',
    vkKey:'swapModelVk' }
  ```
  VK-only (no `modsKey`, no `buttonKey`), like `__cursorLock`. Placing it in the
  Display section - not Keybinds - keeps the swap control beside what it swaps.
- Add `swapModelVk:'0'` to the `kbDefaults` map in `onMount`, so it loads and is
  written live through the existing keybind `live()` path (hot-reload; no restart to
  bind).

### System-wide swallow (accepted tradeoff)

While bound, `swapModelVk` is eaten from the focused app like every other Wind bind,
and per the documented raw-input limitation it still reaches raw-input games. The
user picks a key accordingly.

---

## Testing

Unit (doctest, pure):
- `FlipModel`: render->transform, transform->render, unknown->transform (round-trips).
- `UpdateIniText` replacing `model` in place (extend existing ini_edit coverage).

Manual:
1. Settings: change model, Apply -> Wind restarts silently onto the new model
   (transform-only vs render-only rows switch), no modal, tray returns once, cursor
   restored. Ini `model` matches the running model.
2. Bind the swap hotkey; press while zoomed -> restarts on the other model. Press
   again -> back. Cursor restored each time; tray returns once per press.
3. Press the swap hotkey while idle (not zoomed) -> same flip + restart.
4. Bind/clear the swap key in settings -> takes effect live, no restart; forbidden
   keys refused by capture.
5. Relaunch-failure (e.g. temporarily rename `Wind.exe`): Settings shows the error
   box and reverts the dropdown + ini to the running model; the core logs the failure
   and stays on the current model with the ini reverted.

## Issue / PR mapping

- **Feature A** (WindConfig-only): its own issue -> branch -> PR.
- **Feature B** (core + config + UI): its own issue -> branch -> PR.

Both build on the current model work.
