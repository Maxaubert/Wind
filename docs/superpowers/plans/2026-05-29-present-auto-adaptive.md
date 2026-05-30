# present=auto Adaptive Switching Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add `present=auto` (new default) that runs dcomp normally and auto-falls-back to blt when the windowed-app-over-background-game throttle is detected, switching back when it clears - all at zoom boundaries so it is hitch-free and never resets zoom.

**Architecture:** A pure, doctest-tested `PresentPolicy` state machine (like `lock_detector`) decides the desired present mode from per-tick signals (loop fps, refresh, foreground state). Thin Win32 wiring in `main.cpp`'s tick feeds it those signals and applies its choice via the existing zoom-boundary `setPresentMode` path. The engine and the blt/dcomp paths are unchanged.

**Tech Stack:** C++17, MSVC, doctest (pure policy), Win32 (`GetForegroundWindow`/`GetWindowRect` in the tick).

**Spec:** `docs/superpowers/specs/2026-05-29-present-auto-adaptive-design.md`

---

## File Structure

- Modify: `src/config.h` / `src/config.cpp` / `tests/test_config.cpp` - `present` accepts `auto`, default `auto`.
- Create: `src/present_policy.h` / `src/present_policy.cpp` - pure adaptive policy.
- Create: `tests/test_present_policy.cpp` - doctest coverage.
- Modify: `build.bat` - add `present_policy.cpp` to the test-build source list.
- Modify: `src/main.cpp` - feed the policy, generalize the zoom-boundary apply to a `desiredPresent`.

---

## Task 1: `present=auto` config value (default auto)

**Files:**
- Modify: `src/config.h` (the `present` field, ~line 56)
- Modify: `src/config.cpp` (parse ~line 50; writer ~line 100)
- Test: `tests/test_config.cpp` (replace the existing present test + update the empty-default test)

- [ ] **Step 1: Update the failing test**

In `tests/test_config.cpp`, replace the existing `TEST_CASE("present mode parses with blt default and dcomp opt-in")` with:

```cpp
TEST_CASE("present mode parses; default auto; blt/dcomp/auto accepted") {
    CHECK(ParseConfig("").present == "auto");                 // default
    CHECK(ParseConfig("present=auto\n").present == "auto");
    CHECK(ParseConfig("present=dcomp\n").present == "dcomp");
    CHECK(ParseConfig("present=blt\n").present == "blt");
    CHECK(ParseConfig("present=garbage\n").present == "auto"); // unknown -> auto
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `build.bat test`
Expected: FAIL - `ParseConfig("").present` is still `"blt"`.

- [ ] **Step 3: Change the default in `src/config.h`**

Replace the `present` field declaration:

```cpp
    std::string present = "blt";
```

with:

```cpp
    // Present backend while zoomed (render engine). "auto" (default) = dcomp normally, auto-fall-back
    // to blt when the flip-model composite is throttled (windowed app over a background fullscreen
    // game), re-probing dcomp when that clears. "dcomp" / "blt" pin a fixed mode. #69.
    std::string present = "auto";
```

- [ ] **Step 4: Update the parse in `src/config.cpp`**

Replace the present parse line:

```cpp
            else if (key == "present") c.present = (val == "dcomp") ? "dcomp" : "blt";
```

with:

```cpp
            else if (key == "present") c.present = (val == "blt" || val == "dcomp" || val == "auto") ? val : "auto";
```

- [ ] **Step 5: Update the serialized default in `src/config.cpp`**

Replace the three serialized `present` lines (the comment block + `"present=blt\n"`) with:

```cpp
               "; present: auto=default (dcomp when smooth, auto-fall-back to blt over a background\n"
               ";   fullscreen game); dcomp=force DirectComposition flip-model; blt=force blt-model.\n"
               ";   Change applies on the next zoom-in (no restart). #69\n"
               "present=auto\n"
```

- [ ] **Step 6: Run the test to verify it passes**

Run: `build.bat test`
Expected: PASS (all cases, including the updated present test and the empty-default cases).

- [ ] **Step 7: Commit**

```bash
git add src/config.h src/config.cpp tests/test_config.cpp
git commit -m "feat: present=auto config value, now the default (#69)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 2: Pure `PresentPolicy` state machine + tests

**Files:**
- Create: `src/present_policy.h`
- Create: `src/present_policy.cpp`
- Create: `tests/test_present_policy.cpp`
- Modify: `build.bat` (add `src\present_policy.cpp` to the `:test` source list)

- [ ] **Step 1: Create `src/present_policy.h`**

```cpp
#pragma once
namespace wind {

enum class PresentChoice { Blt, Dcomp };
// Why the last transition happened (for diagnostics logging). None = no transition this update.
enum class PresentReason { None, Throttle, Cue, Backstop };

// Pure policy for present=auto (issue #69). Chooses dcomp normally and falls back to blt when the
// flip-model composite is throttled (a windowed app over a background fullscreen game drops our
// on-screen rate to ~60), then re-probes dcomp when the state likely clears. Pure + hysteretic,
// like LockDetector; main.cpp feeds it per-tick signals and acts on choice() at a zoom boundary.
class PresentPolicy {
public:
    // dt                  : seconds since last tick (real, unclamped).
    // zoomed              : magnifier is zoomed this tick (we only present/measure then).
    // fps                 : measured loop fps this tick (<= 0 if unknown).
    // refreshHz           : detected display refresh (<= 0 falls back to 60).
    // foregroundFullscreen: the foreground window covers the target monitor (game likely in front).
    // foregroundChanged   : the foreground window handle changed since the previous tick.
    PresentChoice update(double dt, bool zoomed, double fps, int refreshHz,
                         bool foregroundFullscreen, bool foregroundChanged);
    PresentChoice choice() const { return choice_; }
    PresentReason lastReason() const { return reason_; }
    void reset();   // back to the optimistic initial state (Dcomp)

private:
    PresentChoice choice_ = PresentChoice::Dcomp;
    PresentReason reason_ = PresentReason::None;
    double belowSecs_  = 0.0;   // sustained time below the throttle threshold (while zoomed, in Dcomp)
    double sinceProbe_ = 0.0;   // time on Blt since the last dcomp probe (drives the backstop)
};

}
```

- [ ] **Step 2: Create `src/present_policy.cpp`**

```cpp
#include "present_policy.h"
namespace wind {
namespace {
constexpr double kThrottleFrac    = 0.7;   // fps below this * refresh counts as throttled
constexpr double kThrottleConfirm = 1.0;   // sustained seconds below threshold -> fall back to Blt
constexpr double kBackstopSecs    = 60.0;  // re-probe dcomp at least this often while on Blt
}

void PresentPolicy::reset() {
    choice_ = PresentChoice::Dcomp;
    reason_ = PresentReason::None;
    belowSecs_ = 0.0;
    sinceProbe_ = 0.0;
}

PresentChoice PresentPolicy::update(double dt, bool zoomed, double fps, int refreshHz,
                                    bool foregroundFullscreen, bool foregroundChanged) {
    if (dt < 0.0) dt = 0.0;
    reason_ = PresentReason::None;
    const double threshold = kThrottleFrac * (refreshHz > 0 ? refreshHz : 60);

    if (choice_ == PresentChoice::Dcomp) {
        // Detect the throttle: sustained low loop fps while actually zoomed. A brief dip resets,
        // so only a real ~1s stall trips it. (Also catches a failed probe: re-throttle -> Blt.)
        if (zoomed && fps > 0.0 && fps < threshold) belowSecs_ += dt;
        else                                        belowSecs_ = 0.0;
        if (belowSecs_ >= kThrottleConfirm) {
            choice_ = PresentChoice::Blt;
            reason_ = PresentReason::Throttle;
            belowSecs_ = 0.0;
            sinceProbe_ = 0.0;
        }
        return choice_;
    }

    // On Blt: re-probe dcomp when a fullscreen window just came to the foreground (game returned)
    // or the backstop elapses. Edge-triggered cue (needs a foreground CHANGE) so we do not probe
    // repeatedly while a fullscreen app sits in front.
    sinceProbe_ += dt;
    const bool cue = (foregroundFullscreen && foregroundChanged);
    if (cue || sinceProbe_ >= kBackstopSecs) {
        choice_ = PresentChoice::Dcomp;
        reason_ = cue ? PresentReason::Cue : PresentReason::Backstop;
        belowSecs_ = 0.0;
        sinceProbe_ = 0.0;
    }
    return choice_;
}

}
```

- [ ] **Step 3: Create `tests/test_present_policy.cpp`**

```cpp
#include "doctest.h"
#include "../src/present_policy.h"
using namespace wind;

TEST_CASE("starts on dcomp") {
    PresentPolicy p;
    CHECK(p.choice() == PresentChoice::Dcomp);
}

TEST_CASE("sustained throttle while zoomed falls back to blt") {
    PresentPolicy p;
    PresentChoice c = PresentChoice::Dcomp;
    for (int i = 0; i < 11; ++i)  // 1.1s > 1.0 confirm; 60 < 0.7*144
        c = p.update(0.1, /*zoomed*/true, /*fps*/60.0, /*refreshHz*/144, false, false);
    CHECK(c == PresentChoice::Blt);
    CHECK(p.lastReason() == PresentReason::Throttle);
}

TEST_CASE("a brief dip does not fall back") {
    PresentPolicy p;
    for (int i = 0; i < 5; ++i) p.update(0.1, true, 60.0, 144, false, false);  // 0.5s low
    PresentChoice c = p.update(0.1, true, 144.0, 144, false, false);            // recovered
    CHECK(c == PresentChoice::Dcomp);
}

TEST_CASE("low fps while NOT zoomed never falls back") {
    PresentPolicy p;
    PresentChoice c = PresentChoice::Dcomp;
    for (int i = 0; i < 50; ++i) c = p.update(0.1, /*zoomed*/false, 10.0, 144, false, false);
    CHECK(c == PresentChoice::Dcomp);
}

TEST_CASE("fullscreen foreground cue re-probes dcomp from blt") {
    PresentPolicy p;
    for (int i = 0; i < 11; ++i) p.update(0.1, true, 60.0, 144, false, false);  // -> Blt
    REQUIRE(p.choice() == PresentChoice::Blt);
    PresentChoice c = p.update(0.1, true, 60.0, 144, /*fgFull*/true, /*fgChanged*/true);
    CHECK(c == PresentChoice::Dcomp);
    CHECK(p.lastReason() == PresentReason::Cue);
}

TEST_CASE("foreground change that is NOT fullscreen does not re-probe") {
    PresentPolicy p;
    for (int i = 0; i < 11; ++i) p.update(0.1, true, 60.0, 144, false, false);  // -> Blt
    PresentChoice c = p.update(0.1, true, 60.0, 144, /*fgFull*/false, /*fgChanged*/true);
    CHECK(c == PresentChoice::Blt);
}

TEST_CASE("backstop re-probes dcomp after the timeout even if still throttled") {
    PresentPolicy p;
    for (int i = 0; i < 11; ++i) p.update(0.1, true, 60.0, 144, false, false);  // -> Blt
    bool reprobed = false;
    for (int i = 0; i < 700 && !reprobed; ++i)
        if (p.update(0.1, true, 60.0, 144, false, false) == PresentChoice::Dcomp) reprobed = true;
    CHECK(reprobed);
    CHECK(p.lastReason() == PresentReason::Backstop);
}

TEST_CASE("a failed probe returns to blt") {
    PresentPolicy p;
    for (int i = 0; i < 11; ++i) p.update(0.1, true, 60.0, 144, false, false);  // -> Blt
    p.update(0.1, true, 60.0, 144, true, true);                                  // cue -> Dcomp probe
    REQUIRE(p.choice() == PresentChoice::Dcomp);
    PresentChoice c = PresentChoice::Dcomp;
    for (int i = 0; i < 11; ++i) c = p.update(0.1, true, 60.0, 144, false, false);  // still throttled
    CHECK(c == PresentChoice::Blt);
}

TEST_CASE("reset returns to optimistic dcomp") {
    PresentPolicy p;
    for (int i = 0; i < 11; ++i) p.update(0.1, true, 60.0, 144, false, false);  // -> Blt
    p.reset();
    CHECK(p.choice() == PresentChoice::Dcomp);
}
```

- [ ] **Step 4: Add the policy to the test build in `build.bat`**

In the `:test` block, append `src\present_policy.cpp` to the source list line (currently ending `... src\lock_detector.cpp src\config_ui\ini_edit.cpp`). The new line:

```bat
   tests\*.cpp ^
   src\transform.cpp src\zoom_controller.cpp src\config.cpp src\cursor_mapper.cpp src\lock_detector.cpp src\present_policy.cpp src\config_ui\ini_edit.cpp ^
```

(`tests\*.cpp` already picks up the new `test_present_policy.cpp`. The app build uses `src\*.cpp`, so `present_policy.cpp` is automatically in `Wind.exe` and the `:check` build.)

- [ ] **Step 5: Run the tests**

Run: `build.bat test`
Expected: PASS - all prior cases plus the 9 new `PresentPolicy` cases. (If it fails to compile because the test build did not pick up `present_policy.cpp`, re-check Step 4.)

- [ ] **Step 6: Commit**

```bash
git add src/present_policy.h src/present_policy.cpp tests/test_present_policy.cpp build.bat
git commit -m "feat: pure PresentPolicy adaptive state machine + tests (#69)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 3: Wire the policy into the tick (main.cpp)

**Files:**
- Modify: `src/main.cpp`

- [ ] **Step 1: Include the policy header**

In `src/main.cpp`, add with the other project includes (after `#include "lock_detector.h"`):

```cpp
#include "present_policy.h"
```

- [ ] **Step 2: Replace the `presentChangePending` field in `TickState`**

In `struct TickState`, replace this line:

```cpp
    bool   presentChangePending = false;   // ini changed `present`; apply at the next zoom boundary
```

with:

```cpp
    // Present-mode switching (applied only at a zoom boundary -> hitch-free, never resets zoom).
    // desiredPresent is the mode we want the engine to be; set by the ini (blt/dcomp) or, when
    // presentAuto, by the adaptive policy. lastForeground feeds the policy's foreground-change cue.
    bool        presentAuto = false;
    PresentMode desiredPresent = PresentMode::Dcomp;
    PresentPolicy presentPolicy;
    HWND        lastForeground = nullptr;
```

- [ ] **Step 3: Update `PresentModeFromCfg` and add a foreground helper**

Replace `PresentModeFromCfg`:

```cpp
static PresentMode PresentModeFromCfg(const Config& c) {
    return (c.present == "dcomp") ? PresentMode::Dcomp : PresentMode::Blt;
}
```

with:

```cpp
// Initial / pinned engine mode for a config. "auto" starts optimistic on dcomp (the policy may
// switch it at runtime); "blt" pins blt; "dcomp" pins dcomp.
static PresentMode PresentModeFromCfg(const Config& c) {
    return (c.present == "blt") ? PresentMode::Blt : PresentMode::Dcomp;
}

// True if the foreground window covers the target monitor (within a small margin) - the auto
// policy's "a fullscreen app is in front" cue (game returned to the foreground).
static bool ForegroundCoversMonitor(HWND fg, const MonitorTarget& mon) {
    if (!fg) return false;
    RECT r;
    if (!GetWindowRect(fg, &r)) return false;
    const int m = 2;
    return r.left <= mon.x + m && r.top <= mon.y + m &&
           r.right >= mon.x + mon.w - m && r.bottom >= mon.y + mon.h - m;
}

// PresentReason -> short tag for the diagnostics log.
static const char* PresentReasonName(PresentReason r) {
    switch (r) {
        case PresentReason::Throttle: return "throttle";
        case PresentReason::Cue:      return "cue";
        case PresentReason::Backstop: return "backstop";
        default:                      return "none";
    }
}
```

- [ ] **Step 4: Update the hot-reload present-change block**

In `RunTick`, replace:

```cpp
            if (nc.present != t.cfg.present) {
                // Present-mode switch rebuilds the swapchain; never do that mid-zoom-frame. Mark it
                // and apply at a zoom boundary (immediately if at 1x, else on the next zoom-in).
                t.presentChangePending = true;
            }
```

with:

```cpp
            if (nc.present != t.cfg.present) {
                // Present-mode switch rebuilds the swapchain; apply at a zoom boundary (handled
                // below). For a fixed mode, pin desiredPresent; for auto, the policy drives it.
                t.presentAuto = (nc.present == "auto");
                t.presentPolicy.reset();                 // start the policy clean on any change
                if (!t.presentAuto) t.desiredPresent = PresentModeFromCfg(nc);
            }
```

- [ ] **Step 5: Run the policy each tick and replace the apply-at-1x block**

In `RunTick`, replace this block (right after `double lvl = t.zoom.level();`):

```cpp
    // Apply a pending present-mode switch only while NOT zoomed (overlay hidden): rebuild is safe
    // and invisible at 1x. If currently zoomed, it is applied on the next zoom-in (below).
    if (t.presentChangePending && lvl <= 1.0 && t.prevLvl <= 1.0) {
        PresentMode want = PresentModeFromCfg(t.cfg);
        if (t.renderEngine.presentMode() != want) t.renderEngine.setPresentMode(want);
        t.presentChangePending = false;
    }
```

with:

```cpp
    // Adaptive present policy (present=auto): feed it this tick's signals; it returns the desired
    // mode. dt is the real loop interval, so fps reflects the actual on-screen-paced rate - the
    // bad-state throttle shows up as a sustained fps drop. Runs at 1x too, so a state-clear while
    // idle is noticed and the next zoom-in comes up as dcomp.
    if (t.presentAuto) {
        HWND fg = GetForegroundWindow();
        bool fgChanged = (fg != t.lastForeground);
        t.lastForeground = fg;
        bool fgFull = ForegroundCoversMonitor(fg, t.mon);
        double fps = dt > 0.0 ? 1.0 / dt : 0.0;
        PresentChoice pc = t.presentPolicy.update(dt, lvl > 1.0, fps, t.hz, fgFull, fgChanged);
        t.desiredPresent = (pc == PresentChoice::Dcomp) ? PresentMode::Dcomp : PresentMode::Blt;
    }

    // Apply a present-mode switch only while NOT zoomed (overlay hidden): the swapchain rebuild is
    // invisible at 1x and never resets the zoom. While zoomed, it waits for the next zoom-in.
    if (lvl <= 1.0 && t.prevLvl <= 1.0 && t.renderEngine.presentMode() != t.desiredPresent) {
        if (t.cfg.diagnostics)
            DiagLog("present: -> %s (%s)", t.desiredPresent == PresentMode::Dcomp ? "dcomp" : "blt",
                    t.presentAuto ? PresentReasonName(t.presentPolicy.lastReason()) : "ini");
        t.renderEngine.setPresentMode(t.desiredPresent);
    }
```

- [ ] **Step 6: Replace the apply-on-zoom-in block**

In `RunTick`, inside `if (lvl > 1.0) { ... if (zoomIn) {`, replace:

```cpp
            if (t.presentChangePending) {
                PresentMode want = PresentModeFromCfg(t.cfg);
                if (t.renderEngine.presentMode() != want) t.renderEngine.setPresentMode(want);
                t.presentChangePending = false;
            }
```

with:

```cpp
            if (t.renderEngine.presentMode() != t.desiredPresent) {
                if (t.cfg.diagnostics)
                    DiagLog("present: -> %s (%s) [zoom-in]",
                            t.desiredPresent == PresentMode::Dcomp ? "dcomp" : "blt",
                            t.presentAuto ? PresentReasonName(t.presentPolicy.lastReason()) : "ini");
                t.renderEngine.setPresentMode(t.desiredPresent);
            }
```

- [ ] **Step 7: Initialize the auto state at startup**

In `wWinMain`, immediately after `TickState ts(...)` is constructed and `ts.hwnd = hwnd;` is set (before `g_tick = &ts;`), add:

```cpp
    ts.presentAuto = (cfg.present == "auto");
    ts.desiredPresent = PresentModeFromCfg(cfg);   // matches the engine's initial mode below
```

(The engine is already initialized with `PresentModeFromCfg(cfg)`, so for `auto` both start on dcomp and no startup switch occurs.)

- [ ] **Step 8: Build + checks (runtime validation deferred to the user)**

Run each, expect exit 0:
- `build.bat` (links Wind.exe)
- `build.bat check` (all sources compile)
- `build.bat test` (doctests pass)

Re-read the diff: `present=blt`/`dcomp` set `presentAuto=false` and pin `desiredPresent` (policy never consulted, so behavior is exactly the fixed-mode behavior from the prior work); only `present=auto` runs the policy; every `setPresentMode` is gated behind `lvl<=1.0 && prevLvl<=1.0` (at-1x) or the `zoomIn` transition, so no mid-zoom switch. The runtime in-game validation (the three scenarios) is the user's, since the render path needs an interactive desktop.

- [ ] **Step 9: Commit**

```bash
git add src/main.cpp
git commit -m "feat: drive present=auto adaptive switching in the tick (#69)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## After the plan

`build.bat test` green, `build.bat check` clean, self-tests still correct in `present=blt` and
`present=dcomp`. Then the user validates `present=auto` in-game with `diagnostics=1`: (a) game
foreground stays dcomp/smooth; (b) windowed app over a running game - first zoom laggy, then the
diag log shows `present: -> blt (throttle)` and subsequent zooms are smooth; (c) bring the game back
to the foreground - diag log shows `present: -> dcomp (cue)` and it returns to dcomp. If the feel is
right, advance/close #69 and record the dcomp/auto gotcha in CLAUDE.md (follow-up).
