# Silent model restart + swap-model hotkey Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make a WindConfig model change Apply silently restart Wind (no confirm modal), and add an optional, unbound-by-default hotkey that alternates the magnifier model (render <-> transform) by restarting Wind onto the flipped model.

**Architecture:** `model` is read once at launch and is not hot-swappable, so both paths persist the new value to `magnifier.ini` and relaunch `Wind.exe`; the existing single-instance eviction handshake (`main.cpp:825`) evicts the incumbent - no new IPC. Feature A is a WindConfig UI change only. Feature B adds a swallowed VK keybind in the core (mirroring `cursorLockVk`), a pure `FlipModel` helper, an in-place ini rewrite via the existing pure `wind::UpdateIniText`, and a self-relaunch.

**Tech Stack:** C++17 / MSVC (`build.bat`), doctest (`build.bat test`), Svelte + Vite config UI (`build.bat config`).

## Global Constraints

- No em-dashes (U+2014) anywhere: code, comments, docs, UI copy. Use en-dashes, commas, or rephrase.
- Pure-logic files (`config.cpp`, `ini_edit.cpp`) MUST NOT include `<windows.h>`; the test build compiles them with `WIND_TESTS` defined.
- Bound keys are swallowed system-wide; forbidden VKs (`IsForbiddenBindVk`) must never be bound - enforced in `ParseConfig` (`sanitizeVk`), the hook (`isBoundKey`), and the config UI capture.
- INI path is always resolved via `wind::ResolveIniPath()`; never hardcode `L"magnifier.ini"`.
- Keep the ini's `model` equal to the RUNNING model at all times (drives the schema `showIf` row gating).
- Feature A and Feature B are separate GitHub issues, branches, and PRs.

---

## Feature A - silent model restart (WindConfig UI only)

One GitHub issue + branch + PR. No C++ changes. No JS unit-test harness exists for this UI (its `npm run test` is Playwright against the running WebView host), so this task is verified by building `WindConfig.exe` and clicking through.

### Task A1: Apply restarts Wind silently; failure reverts to the running model

**Files:**
- Modify: `ui/src/Settings.svelte` (the `apply`/`confirmRestart`/`cancelRestart`/`onMessage` block at lines 38-74, and the modal markup at lines 113-130)

**Interfaces:**
- Consumes: `commit()`, `windowControl('restartWind')`, `setConfig(key, value)`, `onMessage(fn)` - all already imported/defined in `Settings.svelte`.
- Produces: nothing consumed by other tasks.

- [ ] **Step 1: Replace the model-restart state + handlers**

In `ui/src/Settings.svelte`, replace this block (currently lines 38-74):

```js
  let restartOpen = false;
  let restartError = false;
  onMessage(m => { if (m && m.type === 'restartFailed') { restartError = true; restartOpen = true; } });
  function change(keyOrPatch, val) {
```

with (keep the `change` function body that follows unchanged - only the lines above `function change` change, and `change` stays exactly as it is):

```js
  // A model change is applied by writing the ini then relaunching Wind (model is read once at
  // launch, so a hot-reload can't switch it). To the user a deliberate Apply and a hot-reload look
  // identical, so there is no confirm step. restartError still surfaces a relaunch that failed.
  let restartError = false;
  // The model that the LIVE process is running. Captured before commit() overwrites saved.model, so
  // a failed relaunch can revert the ini + dropdown back to it (keeps ini model == running model).
  let runningModel = '';
  onMessage(m => {
    if (m && m.type === 'restartFailed') {
      values = { ...values, model: runningModel };
      saved  = { ...saved,  model: runningModel };
      setConfig('model', runningModel);   // rewrite the ini back to the model still running
      restartError = true;
    }
  });
  function change(keyOrPatch, val) {
```

- [ ] **Step 2: Replace `apply` and delete the confirm handlers**

Replace this block (currently lines 63-74):

```js
  // `model` is read once at Wind launch, so switching it needs a restart. Gate Apply behind a
  // confirmation rather than writing a value the running process will ignore.
  function apply() {
    if (String(values.model) !== String(saved.model)) { restartError = false; restartOpen = true; return; }
    commit();
  }
  // Write the new model FIRST (the relaunched Wind reads it at startup), then relaunch.
  function confirmRestart() { restartOpen = false; commit(); windowControl('restartWind'); }
  // Revert ONLY the model, then commit any other pending edits: cancelling a model switch must not
  // silently discard unrelated changes. Keeping the ini's model equal to the RUNNING model also
  // stops the schema's showIf gating from revealing rows for a model that is not loaded.
  function cancelRestart() { restartOpen = false; values = { ...values, model: saved.model }; commit(); }
```

with:

```js
  // `model` is read once at Wind launch, so switching it writes the ini (model FIRST - the relaunched
  // Wind reads it at startup) then relaunches. No confirm: a restart and a hot-reload look the same.
  function apply() {
    if (String(values.model) !== String(saved.model)) {
      runningModel = saved.model;   // remember what's live before commit() moves saved.model forward
      restartError = false;
      commit();
      windowControl('restartWind');
      return;
    }
    commit();
  }
```

- [ ] **Step 3: Replace the modal markup with the error-only box**

Replace the modal block (currently lines 113-130):

```svelte
  {#if restartOpen}
    <div class="mbackdrop">
      <div class="mbox" role="dialog" aria-modal="true" aria-labelledby="rtitle">
        {#if restartError}
          <h2 id="rtitle">Couldn't restart Wind</h2>
          <p>Wind.exe could not be launched. The magnifier is still running with the previous model.</p>
          <div class="mbtns"><button class="primary" on:click={() => (restartOpen = false)}>Close</button></div>
        {:else}
          <h2 id="rtitle">Restart required</h2>
          <p>Changing the magnifier model requires restarting Wind.</p>
          <div class="mbtns">
            <button on:click={cancelRestart}>Cancel</button>
            <button class="primary" on:click={confirmRestart}>Restart Wind</button>
          </div>
        {/if}
      </div>
    </div>
  {/if}
```

with (error box only; the success path shows no dialog):

```svelte
  {#if restartError}
    <div class="mbackdrop">
      <div class="mbox" role="dialog" aria-modal="true" aria-labelledby="rtitle">
        <h2 id="rtitle">Couldn't restart Wind</h2>
        <p>Wind.exe could not be launched. The magnifier is still running with the previous model.</p>
        <div class="mbtns"><button class="primary" on:click={() => (restartError = false)}>Close</button></div>
      </div>
    </div>
  {/if}
```

- [ ] **Step 4: Build the config UI and verify it compiles**

Run: `build.bat config`
Expected: exit 0; `WindConfig.exe` and `ui/dist/` produced. A Svelte compile error (e.g. a leftover reference to `restartOpen`, `confirmRestart`, or `cancelRestart`) fails the `npm run build` step - if so, remove the stray reference.

- [ ] **Step 5: Manual verification**

Build the app (`build.bat`) and run `Wind.exe`, then run `WindConfig.exe`:
1. Change Magnifier model (Display section), click Apply -> Wind restarts silently (no dialog); the transform-only vs render-only rows switch to match; tray icon returns once; the OS cursor is never left hidden.
2. Change a hot-reloadable key (e.g. Outline thickness), Apply -> applies live, no restart, no dialog.
3. Confirm `magnifier.ini`'s `model=` equals the model now running.

- [ ] **Step 6: Commit**

```bash
git add ui/src/Settings.svelte
git commit -m "feat(config): apply model change with a silent restart, no confirm modal"
```

---

## Feature B - optional swap-model hotkey (core + config + UI)

Separate GitHub issue + branch + PR. Do this on a fresh branch off `main` after Feature A merges (or off `main` independently).

### Task B1: `FlipModel` pure helper

**Files:**
- Modify: `src/config.h` (declare, near the `model` field / other pure decls)
- Modify: `src/config.cpp` (define in the pure section, above the `#ifndef WIND_TESTS` file-I/O block at line 165)
- Test: `tests/test_config_model.cpp`

**Interfaces:**
- Produces: `std::string wind::FlipModel(const std::string& model);` - returns `"render"` when input is `"transform"`, otherwise `"transform"` (so an unknown/corrupt value flips to a valid engine rather than sticking).

- [ ] **Step 1: Write the failing test**

Append to `tests/test_config_model.cpp`:

```cpp
TEST_CASE("FlipModel alternates render and transform") {
    CHECK(FlipModel("render") == "transform");
    CHECK(FlipModel("transform") == "render");
    // round-trips
    CHECK(FlipModel(FlipModel("render")) == "render");
    CHECK(FlipModel(FlipModel("transform")) == "transform");
}

TEST_CASE("FlipModel maps an unknown value to transform") {
    CHECK(FlipModel("bogus") == "transform");
    CHECK(FlipModel("") == "transform");
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `build.bat test`
Expected: FAIL to compile with an error that `FlipModel` is not declared in namespace `wind`.

- [ ] **Step 3: Declare `FlipModel` in `src/config.h`**

Find the pure-function declarations in `src/config.h` (the file already declares `Config ParseConfig(const std::string&);` and `bool IsForbiddenBindVk(int);` - put this beside them, inside `namespace wind`):

```cpp
// render <-> transform. "transform" -> "render"; anything else -> "transform" (so a corrupt model
// value flips to a valid engine). Pure; used by the swap-model hotkey. No I/O, no <windows.h>.
std::string FlipModel(const std::string& model);
```

- [ ] **Step 4: Define `FlipModel` in `src/config.cpp`**

In `src/config.cpp`, inside `namespace wind` and ABOVE the `#ifndef WIND_TESTS` block at line 165 (so it is in the pure/test-visible section), add:

```cpp
std::string FlipModel(const std::string& model) {
    return model == "transform" ? "render" : "transform";
}
```

- [ ] **Step 5: Run the test to verify it passes**

Run: `build.bat test`
Expected: PASS (exit 0); the two new `FlipModel` cases pass.

- [ ] **Step 6: Commit**

```bash
git add src/config.h src/config.cpp tests/test_config_model.cpp
git commit -m "feat(config): add pure FlipModel(render<->transform) helper"
```

### Task B2: `swapModelVk` config field (parse + sanitize + default template)

**Files:**
- Modify: `src/config.h` (add the field near `cursorLockVk` at line 30)
- Modify: `src/config.cpp` (parse at ~line 82, sanitize at ~line 158, default template at ~line 201)
- Test: `tests/test_config.cpp`

**Interfaces:**
- Produces: `Config::swapModelVk` (`int`, default `0`), parsed from ini key `swapModelVk`, sanitized against `IsForbiddenBindVk`.

- [ ] **Step 1: Write the failing test**

Append to `tests/test_config.cpp`:

```cpp
TEST_CASE("swapModelVk parses and defaults to unbound") {
    CHECK(ParseConfig("").swapModelVk == 0);
    CHECK(ParseConfig("swapModelVk=112\n").swapModelVk == 112);   // F1
}

TEST_CASE("swapModelVk rejects a forbidden bind") {
    // 8 = VK_BACK (Backspace) is forbidden; must sanitize to 0.
    CHECK(ParseConfig("swapModelVk=8\n").swapModelVk == 0);
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `build.bat test`
Expected: FAIL to compile - `swapModelVk` is not a member of `wind::Config`.

- [ ] **Step 3: Add the field to `src/config.h`**

Immediately after the `cursorLockVk` field block (ends at line 32), add:

```cpp
    int    swapModelVk      = 0;     // VK code; 0 = unbound. Tap to swap the magnifier model
                                     // (render <-> transform). Swallowed system-wide like
                                     // cursorLockVk (VK only, no modifier). Pressing it restarts
                                     // Wind onto the flipped model (model is not hot-swappable).
```

- [ ] **Step 4: Parse the key in `src/config.cpp`**

After the `cursorLockVk` parse line (line 82: `else if (key == "cursorLockVk") c.cursorLockVk = std::stoi(val);`), add:

```cpp
            else if (key == "swapModelVk")      c.swapModelVk = std::stoi(val);
```

- [ ] **Step 5: Sanitize it in `src/config.cpp`**

After `sanitizeVk(c.cursorLockVk);` (line 158), add:

```cpp
    sanitizeVk(c.swapModelVk);
```

- [ ] **Step 6: Add it to the default-ini template in `src/config.cpp`**

After the `cursorLockVk=0\n` template line (line 201), add:

```cpp
               "; swapModelVk: tap to swap the magnifier model (render <-> transform). Restarts Wind\n"
               ";   onto the other model. VK code; 0=unbound.\n"
               "swapModelVk=0\n"
```

- [ ] **Step 7: Run the test to verify it passes**

Run: `build.bat test`
Expected: PASS (exit 0).

- [ ] **Step 8: Commit**

```bash
git add src/config.h src/config.cpp tests/test_config.cpp
git commit -m "feat(config): add swapModelVk keybind (parse, sanitize, ini template)"
```

### Task B3: track + swallow `swapModelVk` in the input hook

**Files:**
- Modify: `src/input_router.h` (extend `setKeys` signature at lines 61-62; add the `kbSwapModelVk_` member near line 99)
- Modify: `src/input_router.cpp` (`setKeys` at line 90; `isBoundKey` at line 84)
- Modify: `src/main.cpp` (both `setKeys` call sites: startup line 920, hot-reload line 246; hot-reload compare line 245)

This wires the key into the keyboard hook so it is swallowed and its down-state is tracked. It does NOT act on the key yet (that is Task B5). No unit test: `input_router.cpp` is not in the test build (it needs `<windows.h>`); verified by compiling and by a manual swallow check.

**Interfaces:**
- Consumes: `Config::swapModelVk` (Task B2).
- Produces: `InputRouter::setKeys(..., int cursorLockVk, int swapModelVk)` - one new trailing parameter; `swapModelVk` becomes a bound key that `isBoundKey`/`keyPressed` report on.

- [ ] **Step 1: Extend the `setKeys` declaration in `src/input_router.h`**

Replace the declaration (lines 61-62):

```cpp
    void setKeys(int zoomInVk, int zoomInVk2, int zoomOutVk, int zoomOutVk2, int recenterVk,
                 int cursorLockVk);
```

with:

```cpp
    void setKeys(int zoomInVk, int zoomInVk2, int zoomOutVk, int zoomOutVk2, int recenterVk,
                 int cursorLockVk, int swapModelVk);
```

- [ ] **Step 2: Add the member in `src/input_router.h`**

After `std::atomic<int> kbCursorLockVk_{0};` (line 99), add:

```cpp
    std::atomic<int> kbSwapModelVk_{0};
```

- [ ] **Step 3: Store it in `setKeys` in `src/input_router.cpp`**

In `setKeys` (line 90), update the signature and add the store. Replace:

```cpp
void InputRouter::setKeys(int zoomInVk, int zoomInVk2, int zoomOutVk, int zoomOutVk2, int recenterVk,
                          int cursorLockVk) {
    kbZoomInVk_.store(zoomInVk,    std::memory_order_relaxed);
    kbZoomInVk2_.store(zoomInVk2,  std::memory_order_relaxed);
    kbZoomOutVk_.store(zoomOutVk,  std::memory_order_relaxed);
    kbZoomOutVk2_.store(zoomOutVk2,std::memory_order_relaxed);
    kbRecenterVk_.store(recenterVk,std::memory_order_relaxed);
    kbCursorLockVk_.store(cursorLockVk, std::memory_order_relaxed);
```

with:

```cpp
void InputRouter::setKeys(int zoomInVk, int zoomInVk2, int zoomOutVk, int zoomOutVk2, int recenterVk,
                          int cursorLockVk, int swapModelVk) {
    kbZoomInVk_.store(zoomInVk,    std::memory_order_relaxed);
    kbZoomInVk2_.store(zoomInVk2,  std::memory_order_relaxed);
    kbZoomOutVk_.store(zoomOutVk,  std::memory_order_relaxed);
    kbZoomOutVk2_.store(zoomOutVk2,std::memory_order_relaxed);
    kbRecenterVk_.store(recenterVk,std::memory_order_relaxed);
    kbCursorLockVk_.store(cursorLockVk, std::memory_order_relaxed);
    kbSwapModelVk_.store(swapModelVk, std::memory_order_relaxed);
```

(Leave the rest of the function - the per-key clear loop - unchanged.)

- [ ] **Step 4: Include it in `isBoundKey` in `src/input_router.cpp`**

Replace the final line of `isBoundKey` (line 84):

```cpp
        || vk == kbCursorLockVk_.load(std::memory_order_relaxed);
```

with:

```cpp
        || vk == kbCursorLockVk_.load(std::memory_order_relaxed)
        || vk == kbSwapModelVk_.load(std::memory_order_relaxed);
```

- [ ] **Step 5: Update the startup `setKeys` call in `src/main.cpp`**

Replace line 920:

```cpp
    g_input.setKeys(cfg.zoomInVk, cfg.zoomInVk2, cfg.zoomOutVk, cfg.zoomOutVk2, cfg.recenterVk, cfg.cursorLockVk);
```

with:

```cpp
    g_input.setKeys(cfg.zoomInVk, cfg.zoomInVk2, cfg.zoomOutVk, cfg.zoomOutVk2, cfg.recenterVk,
                    cfg.cursorLockVk, cfg.swapModelVk);
```

- [ ] **Step 6: Update the hot-reload compare + `setKeys` call in `src/main.cpp`**

Replace the hot-reload block (lines 243-247):

```cpp
            if (nc.zoomInVk != t.cfg.zoomInVk || nc.zoomOutVk != t.cfg.zoomOutVk
             || nc.zoomInVk2 != t.cfg.zoomInVk2 || nc.zoomOutVk2 != t.cfg.zoomOutVk2
             || nc.recenterVk != t.cfg.recenterVk || nc.cursorLockVk != t.cfg.cursorLockVk) {
                g_input.setKeys(nc.zoomInVk, nc.zoomInVk2, nc.zoomOutVk, nc.zoomOutVk2, nc.recenterVk, nc.cursorLockVk);
            }
```

with:

```cpp
            if (nc.zoomInVk != t.cfg.zoomInVk || nc.zoomOutVk != t.cfg.zoomOutVk
             || nc.zoomInVk2 != t.cfg.zoomInVk2 || nc.zoomOutVk2 != t.cfg.zoomOutVk2
             || nc.recenterVk != t.cfg.recenterVk || nc.cursorLockVk != t.cfg.cursorLockVk
             || nc.swapModelVk != t.cfg.swapModelVk) {
                g_input.setKeys(nc.zoomInVk, nc.zoomInVk2, nc.zoomOutVk, nc.zoomOutVk2, nc.recenterVk,
                                nc.cursorLockVk, nc.swapModelVk);
            }
```

- [ ] **Step 7: Build and verify it compiles**

Run: `build.bat`
Expected: exit 0; `Wind.exe` produced. (A missing-argument error at either `setKeys` call site means a call site was not updated.)

- [ ] **Step 8: Commit**

```bash
git add src/input_router.h src/input_router.cpp src/main.cpp
git commit -m "feat(input): track and swallow swapModelVk in the keyboard hook"
```

### Task B4: link the pure ini editor into `Wind.exe`

**Files:**
- Modify: `build.bat` (the normal app build at lines 33-34 and the uiaccess build at lines 48-49)

The core needs `wind::UpdateIniText` (defined in `src/config_ui/ini_edit.cpp`). `Wind.exe` compiles `src\*.cpp`, which does not glob the `config_ui` subdirectory, so add the file explicitly. `ini_edit.cpp` is pure (no `<windows.h>`), so it links cleanly.

**Interfaces:**
- Produces: `wind::UpdateIniText` and `wind::ReadIniValues` available to link in `Wind.exe`.

- [ ] **Step 1: Add `ini_edit.cpp` to the normal app build**

In `build.bat`, replace the source line of the normal build (line 34):

```
   src\*.cpp src\wind.res ^
```

with:

```
   src\*.cpp src\config_ui\ini_edit.cpp src\wind.res ^
```

- [ ] **Step 2: Add `ini_edit.cpp` to the uiaccess build**

In `build.bat`, replace the source line of the uiaccess build (line 49):

```
   src\*.cpp src\wind.res ^
```

with:

```
   src\*.cpp src\config_ui\ini_edit.cpp src\wind.res ^
```

- [ ] **Step 3: Build and verify it compiles + links**

Run: `build.bat`
Expected: exit 0; `Wind.exe` produced with no duplicate-symbol or unresolved-symbol errors.

- [ ] **Step 4: Commit**

```bash
git add build.bat
git commit -m "build: link the pure ini_edit into Wind.exe for the swap-model hotkey"
```

### Task B5: swap action in the tick loop (flip ini + relaunch)

**Files:**
- Modify: `src/main.cpp` (add a `swapKeyWasDown` field to `TickState` near line 123; add a `SwapModelAndRelaunch` helper near the single-instance code ~line 815; add the rising-edge handler in `RunTick` after the recenter block at line 315; add `#include "config_ui/ini_edit.h"` and `<fstream>` at the top if not present)

No unit test (touches `main.cpp` / `<windows.h>`, not in the test build). Verified by build + manual.

**Interfaces:**
- Consumes: `wind::FlipModel` (B1), `Config::swapModelVk` (B2), the swallowed key state from B3, `wind::UpdateIniText` linked by B4, `t.iniPath` (`TickState.iniPath`, line 119), the `keyDown` lambda in `RunTick` (line 269).
- Produces: nothing consumed by later tasks.

- [ ] **Step 1: Confirm the needed includes at the top of `src/main.cpp`**

Ensure these appear with the other includes at the top of `src/main.cpp` (add whichever is missing):

```cpp
#include <fstream>
#include "config_ui/ini_edit.h"   // wind::UpdateIniText - flip the model key in place
```

- [ ] **Step 2: Add the edge-detect field to `TickState`**

In `TickState` (after `bool recenterKeyWasDown = false;` at line 123), add:

```cpp
    bool   swapKeyWasDown = false;             // edge-detect the swapModelVk key (model swap + restart)
```

- [ ] **Step 3: Add the `SwapModelAndRelaunch` helper**

Add this free function above `RunTick` (a natural spot is just after `TerminateOtherWind`/the single-instance helpers, ~line 815). It rewrites the ini's `model` in place, then relaunches `Wind.exe`; the new instance's eviction handshake stops this one. On a relaunch failure it reverts the ini so the file never disagrees with the still-running model.

```cpp
// Swap the magnifier model (render <-> transform) by rewriting magnifier.ini's `model` in place and
// relaunching Wind.exe: model is read once at launch, so a restart is the only way to switch it. The
// freshly launched instance runs the single-instance eviction handshake (signals Wind_QuitRequest),
// so THIS instance exits cleanly (cursor restore, tray removal, clip release) - no new IPC. On a
// launch failure the ini is reverted so it never claims a model the live process is not running.
static void SwapModelAndRelaunch(const std::wstring& iniPath, const std::string& currentModel) {
    const std::string flipped = wind::FlipModel(currentModel);
    // Read the ini text (UTF-8). If it can't be read, bail rather than clobber it.
    std::string text;
    { std::ifstream f(iniPath, std::ios::binary);
      if (!f) { wind::Log(wind::LogLevel::Warn, "swap", "ini read failed; swap skipped"); return; }
      text.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()); }
    const std::string updated = wind::UpdateIniText(text, "model", flipped);
    { std::ofstream out(iniPath, std::ios::binary | std::ios::trunc);
      if (!out) { wind::Log(wind::LogLevel::Warn, "swap", "ini write failed; swap skipped"); return; }
      out << updated; }
    wind::Log(wind::LogLevel::Info, "swap", "model %s -> %s; relaunching",
              currentModel.c_str(), flipped.c_str());
    wchar_t exePath[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, exePath, MAX_PATH)) {
        wind::Log(wind::LogLevel::Warn, "swap", "GetModuleFileName failed; reverting ini");
        std::ofstream out(iniPath, std::ios::binary | std::ios::trunc); out << text; return;
    }
    HINSTANCE r = ShellExecuteW(nullptr, L"open", exePath, nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(r) <= 32) {
        // Relaunch failed: stay on the current model and put the ini back so it matches reality.
        wind::Log(wind::LogLevel::Warn, "swap", "relaunch failed (%lld); reverting ini",
                  static_cast<long long>(reinterpret_cast<INT_PTR>(r)));
        std::ofstream out(iniPath, std::ios::binary | std::ios::trunc); out << text;
    }
    // On success, do nothing else: the new instance signals Wind_QuitRequest and we exit via the
    // normal quit path. (No self-terminate here - the handshake owns the teardown.)
}
```

- [ ] **Step 4: Add the rising-edge handler in `RunTick`**

In `RunTick`, immediately AFTER the recenter block (after `t.recenterKeyWasDown = recenterDown;` at line 315), add:

```cpp
    // Swap the magnifier model on the swapModelVk rising edge (works zoomed or idle). This writes the
    // flipped model to the ini and relaunches Wind; the relaunch evicts this instance, so nothing
    // after this in the tick matters once it fires. Processed unconditionally (not gated on zoom).
    bool swapDown = keyDown(t.cfg.swapModelVk);
    if (swapDown && !t.swapKeyWasDown) SwapModelAndRelaunch(t.iniPath, t.cfg.model);
    t.swapKeyWasDown = swapDown;
```

- [ ] **Step 5: Build and verify it compiles + links**

Run: `build.bat`
Expected: exit 0; `Wind.exe` produced. (An unresolved `UpdateIniText`/`FlipModel` means B1 or B4 is incomplete; a `wind::Log` signature error means match the existing `wind::Log(LogLevel, category, fmt, ...)` calls already in `main.cpp`.)

- [ ] **Step 6: Manual verification (temporary bind via ini)**

Set `swapModelVk=112` (F1) in `magnifier.ini`, run `Wind.exe`. Zoom in, then press F1:
- Wind restarts onto the other model within ~1s (transform vs render behaviour differs; check `%LOCALAPPDATA%\Wind\logs\wind-core.log` for the `swap` line and the startup model).
- Press F1 again -> flips back.
- Press F1 while NOT zoomed -> still flips + restarts.
- Cursor is restored across each restart; the tray icon returns exactly once per swap.
- `magnifier.ini`'s `model=` matches the model now running.
Reset `swapModelVk=0` afterwards.

- [ ] **Step 7: Commit**

```bash
git add src/main.cpp
git commit -m "feat: swap magnifier model on the swapModelVk hotkey (flip ini + relaunch)"
```

### Task B6: expose the swap hotkey in the config UI

**Files:**
- Modify: `ui/src/settings-schema.js` (add a `keybind` row in the Display section, right after the `model` select at line 31)
- Modify: `ui/src/Settings.svelte` (add `swapModelVk:'0'` to the `kbDefaults` map at lines 20-25)

No JS unit tests; verified by building and clicking through.

**Interfaces:**
- Consumes: the `keybind` row type, `vkKey` field, and the `live()` write path already used by `__cursorLock`.
- Produces: a "Swap model hotkey" bind row writing ini key `swapModelVk`.

- [ ] **Step 1: Add the schema row**

In `ui/src/settings-schema.js`, in the Display section, immediately after the `model` select object (the one ending `options:['render','transform'], def:'render' },` at line 31), insert:

```js
    { key:'__swapModel', type:'keybind', label:'Swap model hotkey',
      desc:'Press to switch between Render and Transform (restarts Wind). Right-click to clear.',
      vkKey:'swapModelVk' },
```

- [ ] **Step 2: Add the default to `kbDefaults`**

In `ui/src/Settings.svelte`, add `swapModelVk:'0'` to the `kbDefaults` object (lines 20-25). Replace:

```js
                         hideCursorVk:'0', hideCursorMods:'0',
                         quickZoomVk:'112', quickZoomMods:'0' };
```

with:

```js
                         hideCursorVk:'0', hideCursorMods:'0',
                         swapModelVk:'0',
                         quickZoomVk:'112', quickZoomMods:'0' };
```

- [ ] **Step 3: Build the config UI**

Run: `build.bat config`
Expected: exit 0; `WindConfig.exe` + `ui/dist/` produced.

- [ ] **Step 4: Manual verification**

Run `Wind.exe`, then `WindConfig.exe`. In the Display section, under Magnifier model, there is a "Swap model hotkey" row:
1. It shows the current binding (Unbound by default). Click it, press a key (e.g. F8) -> it captures and writes `swapModelVk`. Right-click -> clears back to unbound. A forbidden key (Backspace, click, Win) is refused by the capture.
2. With F8 bound, press F8 anywhere -> Wind swaps model + restarts (Feature B end-to-end through the real bind).
3. Confirm binding the key takes effect live (no restart to bind - only pressing it restarts).

- [ ] **Step 5: Commit**

```bash
git add ui/src/settings-schema.js ui/src/Settings.svelte
git commit -m "feat(config): add the Swap model hotkey bind under the model selector"
```

### Task B7: document the swap hotkey

**Files:**
- Modify: `README.md` (keybind/keys section)
- Modify: `CLAUDE.md` (the INPUT SWALLOWING gotcha lists the VK-only swallowed binds - add `swapModelVk`)

- [ ] **Step 1: Update `README.md`**

Find where the keybinds/hotkeys are listed (search `recenterVk` or "Inspect") and add a line describing the swap-model hotkey: an optional, unbound-by-default key that alternates the magnifier model (render <-> transform); pressing it restarts Wind onto the other model; bind it under Display -> Magnifier model in the settings.

- [ ] **Step 2: Update `CLAUDE.md`**

In the INPUT SWALLOWING gotcha, where it enumerates the VK-only swallowed binds (`recenterVk`, `cursorLockVk`), add `swapModelVk` as another VK-only swallowed tap key whose press writes the flipped `model` to the ini and relaunches Wind via the single-instance eviction handshake.

- [ ] **Step 3: Commit**

```bash
git add README.md CLAUDE.md
git commit -m "docs: document the swap-model hotkey"
```

---

## Self-Review notes

- **Spec coverage:** Feature A -> Task A1. Feature B config field -> B2; `FlipModel` -> B1; swallow/track -> B3; ini-editor link -> B4; flip+relaunch+revert-on-failure -> B5; UI bind row + kbDefaults -> B6; docs -> B7. Testing section: `FlipModel` doctest (B1), `swapModelVk` parse/sanitize doctest (B2), `UpdateIniText` model replacement is already covered by `tests/test_ini_edit.cpp`; manual steps live in A1/B5/B6.
- **Invariant (ini model == running model):** A1 reverts on `restartFailed`; B5 reverts the ini on relaunch failure. Both preserve it.
- **Type consistency:** `setKeys` gains exactly one trailing `int swapModelVk` param; both call sites (startup, hot-reload) and the hot-reload compare are updated in B3. `FlipModel` signature identical across B1 (def) and B5 (use). `SwapModelAndRelaunch(const std::wstring&, const std::string&)` defined and called only in B5.
- **`wind::Log`:** B5 uses the existing `wind::Log(LogLevel, category, fmt, ...)` form already used throughout `main.cpp`; match the actual signature when implementing.
