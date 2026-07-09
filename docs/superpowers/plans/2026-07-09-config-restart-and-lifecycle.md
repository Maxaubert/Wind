# Config Restart Prompt + Lifecycle Coupling Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Changing the magnifier `model` prompts to restart and actually restarts Wind; and the config window closes whenever the magnifier exits.

**Architecture:** Feature A needs no new IPC - Wind's startup already evicts a running instance via the `Local\Wind_QuitRequest` handshake, so "restart" is just relaunching `Wind.exe` through the launch path WindConfig already has. Feature B polls the existing `WindRunning()` helper on a 1s `WM_TIMER`, guarded by pure, unit-tested logic that requires two consecutive misses and only arms after Wind has been seen alive.

**Tech Stack:** C++17 / MSVC (`build.bat`), Win32 + WebView2 config host, Svelte 4 UI, doctest for pure logic.

Spec: `docs/superpowers/specs/2026-07-09-config-restart-and-lifecycle-design.md`

## Global Constraints

- Build: `build.bat config` (config host + UI), `build.bat test` (doctest). Both must exit 0.
- **Never `PostMessage` from WindConfig to Wind.** The deployed `Wind.exe` is UIAccess; UIPI silently swallows window messages from the normal-IL config host. Use kernel objects (named events) or process launch.
- Test build compiles `tests\*.cpp` plus an explicit list of **pure-logic** sources (`build.bat:79-82`). Anything unit-tested must not include `<windows.h>`.
- Style: no em-dashes. Use en-dashes, commas, or rephrase.
- Feature A lands on `feat/transform-model` (issue #126). Feature B gets its own issue and branch, after #126.

### Deviation from the spec (deliberate)

The spec says "if Wind is not running, skip the modal and just save." That state is unreachable on the Settings page: WindConfig launches Wind at startup when it is not running (`src/config_ui/main.cpp:260-262`), and Feature B closes the window if Wind later exits. Querying Wind's liveness from the UI would add a message round-trip for an impossible case. **Dropped.** The modal is shown whenever `model` changes.

---

## File Structure

- `src/config_ui/main.cpp` - add `LaunchWind()` helper (factored from the existing startup launch), a `restartWind` window action, and the Feature B watchdog timer.
- `src/config_ui/wind_watchdog.h` (new) - header-only, pure, no `<windows.h>`. The close decision.
- `tests/test_wind_watchdog.cpp` (new) - doctest for the above.
- `ui/src/Settings.svelte` - restart modal + Apply gating.

---

## Task 1: `restartWind` native action (Feature A, issue #126)

**Files:**
- Modify: `src/config_ui/main.cpp` (add `LaunchWind()` near `WindRunning()` at :32-43; refactor :255-263; extend `HandleWebMessage` :151-163)

**Interfaces:**
- Produces: `static bool LaunchWind();` - returns false when `ShellExecuteW` fails (result <= 32).
- Produces: web message `{"type":"window","action":"restartWind"}` handled natively.
- Produces: native -> UI message `{"type":"restartFailed"}` posted when the launch fails.

- [ ] **Step 1: Add the `LaunchWind()` helper**

Insert immediately after `WindRunning()` (after line 43):

```cpp
// Launch the magnifier that sits next to us. Returns false when ShellExecuteW failed (<= 32).
// Relaunching while Wind is ALREADY running is how a model switch restarts it: the new instance
// finds the single-instance mutex held, signals Local\Wind_QuitRequest to the incumbent, waits for
// it to exit cleanly, then takes over (src/main.cpp:801-817). No extra IPC is needed here.
static bool LaunchWind() {
    std::wstring windExe = ExeDir() + L"\\Wind.exe";
    HINSTANCE r = ShellExecuteW(nullptr, L"open", windExe.c_str(), nullptr, nullptr, SW_SHOW);
    return reinterpret_cast<INT_PTR>(r) > 32;
}
```

- [ ] **Step 2: Route the two existing launch sites through it**

Replace lines 255-263 (the `if (!onboarded) { ... } else if (!WindRunning()) { ... }` block) with:

```cpp
        if (!onboarded) {
            if (LaunchWind()) { if (mtx) CloseHandle(mtx); return 0; }
            onboard = true;   // couldn't launch Wind - run onboarding in THIS window, not the config page
        } else if (!WindRunning()) {
            // Set up, but the magnifier isn't running: start it, then continue to the config page.
            LaunchWind();
        }
```

Delete the now-unused `std::wstring windExe = ExeDir() + L"\\Wind.exe";` line above them.

- [ ] **Step 3: Handle the `restartWind` action**

In `HandleWebMessage`, inside the `type == "window"` branch, after the `quitWind` case (line 163), add:

```cpp
        else if (action == "restartWind") {
            // `model` is read once at launch, so switching it needs a real restart. The UI has
            // already written the new value to the ini. Just launch Wind again: the new instance
            // evicts the incumbent via the Local\Wind_QuitRequest handshake. On failure the
            // incumbent is untouched (it is only ever signalled BY a successfully started instance),
            // so report back rather than leaving the user with a silently ignored button.
            if (!LaunchWind()) wv->PostWebMessageAsJson(L"{\"type\":\"restartFailed\"}");
        }
```

- [ ] **Step 4: Build**

Run: `build.bat config`
Expected: exit code 0, `WindConfig.exe` produced.

- [ ] **Step 5: Commit**

```bash
git add src/config_ui/main.cpp
git commit -m "feat(config): restartWind action relaunches Wind to apply a model change (#126)"
```

---

## Task 2: Restart-required modal (Feature A, issue #126)

**Files:**
- Modify: `ui/src/Settings.svelte` (imports :4; script :56-59; markup; styles)

**Interfaces:**
- Consumes: `windowControl('restartWind')` and `{"type":"restartFailed"}` from Task 1.
- Consumes: existing `apply()` / `saved` / `values` staging (`Settings.svelte:56-62`).

Note: `apply()` flushes staged edits, so the modal gates **Apply**, not the select's `on:change`.

- [ ] **Step 1: Import `onMessage`**

Change line 4 to:

```js
  import { getConfig, setConfig, openIni, exportDiagnostics, windowControl, onMessage } from './bridge.js';
```

- [ ] **Step 2: Add modal state and listen for a failed restart**

Add inside `<script>`, after the `live()` function (after line 37):

```js
  let restartOpen = false;
  let restartError = false;
  onMessage(m => { if (m && m.type === 'restartFailed') { restartError = true; restartOpen = true; } });
```

- [ ] **Step 3: Split `apply()` into `commit()` + gated `apply()`**

Replace `apply()` (lines 56-59) with:

```js
  function commit() {
    for (const k of Object.keys(values)) if (String(values[k]) !== String(saved[k])) setConfig(k, values[k]);
    saved = { ...values };
  }
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

- [ ] **Step 4: Add the modal markup**

Add immediately before the final `</div>` that closes `.win` (end of markup, before `<style>`):

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

- [ ] **Step 5: Add the modal styles**

Append inside the existing `<style>` block:

```css
  .mbackdrop { position: fixed; inset: 0; background: rgba(0,0,0,.45);
               display: flex; align-items: center; justify-content: center; z-index: 50; }
  .mbox { background: var(--card, #1e1e22); color: var(--fg, #eee); border-radius: 10px;
          padding: 20px 22px; width: 380px; box-shadow: 0 12px 40px rgba(0,0,0,.5); }
  .mbox h2 { margin: 0 0 8px; font-size: 15px; }
  .mbox p { margin: 0 0 18px; font-size: 13px; opacity: .85; line-height: 1.45; }
  .mbtns { display: flex; gap: 8px; justify-content: flex-end; }
  .mbtns button { padding: 7px 14px; border-radius: 6px; font-size: 13px; cursor: pointer;
                  border: 1px solid var(--bd, #3a3a40); background: transparent; color: inherit; }
  .mbtns button.primary { background: #5b5bd6; border-color: #5b5bd6; color: #fff; }
```

- [ ] **Step 6: Build**

Run: `build.bat config`
Expected: exit code 0 (npm build of `ui` succeeds, `ui/dist` produced).

- [ ] **Step 7: Manual verification**

Deploy with `tools/uiaccess_setup.ps1` (elevated), launch `C:\Program Files\Wind\Wind.exe` from a normal shell, open Settings from the tray.

1. Change model, press Apply, press **Cancel** -> dropdown reverts to the running model; `magnifier.ini` `model` is unchanged; Wind keeps running (tray icon never disappears).
2. Change an unrelated key (e.g. `outlineThickness`) AND the model, press Apply, press **Cancel** -> model reverts, the unrelated key IS saved.
3. Change model, press Apply, press **Restart Wind** -> Wind exits and relaunches once; the tray icon returns exactly once; `model` in the ini matches the running model; transform-only rows appear/disappear in the UI accordingly.
4. Change only a hot-reloadable key -> no modal, applies live.

- [ ] **Step 8: Commit**

```bash
git add ui/src/Settings.svelte
git commit -m "feat(config): confirm + restart Wind when the magnifier model changes (#126)"
```

---

## Task 3: Pure close-decision helper (Feature B, own issue)

**Files:**
- Create: `src/config_ui/wind_watchdog.h`
- Create: `tests/test_wind_watchdog.cpp`

**Interfaces:**
- Produces: `bool wind::ShouldCloseOnWindGone(bool running, bool& armed, int& misses);`

Header-only and free of `<windows.h>`, so it compiles into the `WIND_TESTS` build without adding a source file to `build.bat:81`.

- [ ] **Step 1: Write the failing test**

Create `tests/test_wind_watchdog.cpp`:

```cpp
#include "doctest.h"
#include "../src/config_ui/wind_watchdog.h"

using wind::ShouldCloseOnWindGone;

TEST_CASE("never closes before Wind has been seen running") {
    bool armed = false; int misses = 0;
    CHECK_FALSE(ShouldCloseOnWindGone(false, armed, misses));
    CHECK_FALSE(ShouldCloseOnWindGone(false, armed, misses));
    CHECK_FALSE(ShouldCloseOnWindGone(false, armed, misses));
    CHECK_FALSE(armed);
}

TEST_CASE("one miss does not close (a Toolhelp snapshot failure also reports false)") {
    bool armed = false; int misses = 0;
    CHECK_FALSE(ShouldCloseOnWindGone(true, armed, misses));
    CHECK(armed);
    CHECK_FALSE(ShouldCloseOnWindGone(false, armed, misses));
}

TEST_CASE("two consecutive misses close") {
    bool armed = false; int misses = 0;
    ShouldCloseOnWindGone(true, armed, misses);
    CHECK_FALSE(ShouldCloseOnWindGone(false, armed, misses));
    CHECK(ShouldCloseOnWindGone(false, armed, misses));
}

TEST_CASE("a running observation between misses resets the counter") {
    bool armed = false; int misses = 0;
    ShouldCloseOnWindGone(true, armed, misses);
    CHECK_FALSE(ShouldCloseOnWindGone(false, armed, misses));
    CHECK_FALSE(ShouldCloseOnWindGone(true, armed, misses));
    CHECK_EQ(misses, 0);
    CHECK_FALSE(ShouldCloseOnWindGone(false, armed, misses));   // needs two again
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `build.bat test`
Expected: FAIL to compile - `cannot open include file: '../src/config_ui/wind_watchdog.h'`

- [ ] **Step 3: Write the implementation**

Create `src/config_ui/wind_watchdog.h`:

```cpp
// Pure decision logic for "the magnifier is gone, so close the config window".
// Deliberately free of <windows.h> so it compiles into the WIND_TESTS build.
#pragma once
namespace wind {

// running: this poll's WindRunning() result.
// armed:   caller-owned. Latches true once Wind has been observed running.
// misses:  caller-owned. Consecutive false observations since the last true.
//
// Two guards, both load-bearing:
//  - WindRunning() returns false when its process snapshot FAILS, not only when Wind is absent
//    (src/config_ui/main.cpp:35). One transient false must never close the user's window.
//  - WindConfig can legitimately show a window while Wind is down: a failed Wind.exe launch falls
//    back to onboarding (src/config_ui/main.cpp:246). Closing then would hide that very error.
inline bool ShouldCloseOnWindGone(bool running, bool& armed, int& misses) {
    if (running) { armed = true; misses = 0; return false; }
    if (!armed) return false;
    return ++misses >= 2;
}

}  // namespace wind
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `build.bat test`
Expected: exit 0, all doctest cases pass.

- [ ] **Step 5: Commit**

```bash
git add src/config_ui/wind_watchdog.h tests/test_wind_watchdog.cpp
git commit -m "feat(config): pure close-on-Wind-gone decision helper + tests"
```

---

## Task 4: Close the config window when Wind exits (Feature B, own issue)

**Files:**
- Modify: `src/config_ui/main.cpp` (include; `WndProc` :179-223; `SetTimer` after window creation ~:273)

**Interfaces:**
- Consumes: `wind::ShouldCloseOnWindGone` (Task 3), `WindRunning()` (:32).

- [ ] **Step 1: Include the helper and declare the timer constants**

Add the include alongside the other project includes at the top of `src/config_ui/main.cpp`:

```cpp
#include "wind_watchdog.h"
```

Add above `WndProc` (before line 179):

```cpp
// Poll for the magnifier's exit (Ctrl+Alt+Q, tray Quit, or a crash) and take this window down with
// it: "the config window should not exist if the magnifier is offline". A poll rather than an event
// because a crash never signals an event, and rather than a process handle wait because that needs
// OpenProcess against a higher-integrity UIAccess process. <= 1s latency is imperceptible here.
static const UINT_PTR kWindWatchTimerId = 0xB100;
static const UINT     kWindWatchPeriodMs = 1000;
```

- [ ] **Step 2: Handle `WM_TIMER` and kill the timer on destroy**

In `WndProc`, add immediately before the `WM_DESTROY` line (line 221):

```cpp
    if (m == WM_TIMER && w == kWindWatchTimerId) {
        static bool armed = false;
        static int  misses = 0;
        if (wind::ShouldCloseOnWindGone(WindRunning(), armed, misses))
            PostMessageW(h, WM_CLOSE, 0, 0);
        return 0;
    }
```

Replace the `WM_DESTROY` line with:

```cpp
    if (m == WM_DESTROY) { KillTimer(h, kWindWatchTimerId); PostQuitMessage(0); return 0; }
```

- [ ] **Step 3: Start the timer once the window exists**

After the `CreateWindowExW` call and its `hwnd` null check (after line 273), add:

```cpp
    SetTimer(hwnd, kWindWatchTimerId, kWindWatchPeriodMs, nullptr);
```

- [ ] **Step 4: Build**

Run: `build.bat config` then `build.bat test`
Expected: both exit 0.

- [ ] **Step 5: Manual verification**

Deploy, launch Wind, open Settings from the tray.

1. Press Ctrl+Alt+Q -> the config window closes within ~1s.
2. Reopen Settings, right-click tray -> Quit -> the config window closes within ~1s.
3. Reopen Settings, kill `Wind.exe` from Task Manager -> the config window closes within ~1s (crash path).
4. Quit Wind entirely, then launch `WindConfig.exe` directly -> Wind launches, the config window opens and **stays open**.
5. Change the model and confirm **Restart Wind** -> the config window survives the restart (the relaunched Wind appears in the process list before the incumbent exits, so `WindRunning()` never observes a gap; the two-miss guard covers any residual jitter).

- [ ] **Step 6: Commit**

```bash
git add src/config_ui/main.cpp
git commit -m "feat(config): close the config window when the magnifier exits"
```

---

## Self-Review

**Spec coverage:**
- Feature A modal + Confirm/Cancel -> Task 2. Cancel writes no model -> Task 2 Step 3.
- Feature A restart mechanism (relaunch, no new IPC) -> Task 1.
- Feature A error handling (ShellExecute <= 32) -> Task 1 Step 3 + Task 2 Step 4.
- Feature A "skip modal when Wind not running" -> deliberately dropped, see Global Constraints.
- Feature B 1s WM_TIMER poll -> Task 4.
- Feature B two guards + unit test -> Task 3.
- Feature B / A interaction (no gap mid-restart) -> Task 4 Step 5 case 5.
- Non-goal: X button unchanged. No task touches it. Correct.

**Placeholders:** none. Every code step shows complete code.

**Type consistency:** `LaunchWind()` returns `bool` and is used as `bool` in Tasks 1 and 4's neighbours. `ShouldCloseOnWindGone(bool, bool&, int&) -> bool` matches between Task 3's header, its test, and Task 4's call site. Message names `restartWind` / `restartFailed` match between Task 1 (native) and Task 2 (UI).
