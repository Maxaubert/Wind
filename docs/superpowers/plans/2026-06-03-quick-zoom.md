# Quick Zoom (double-tap toggle) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Double-tapping either zoom key instantly toggles the magnifier between 1.0x ("0%") and a remembered zoom level (or a configurable default), remembering the level being left only when it is above 200%.

**Architecture:** All decision logic is pure and unit-tested in `src/zoom_controller.{h,cpp}` (already compiled into both the app and test builds): a `QuickZoomDetector` that fires on two quick taps of the same channel, a `ZoomController::setLevel` instant snap, and an `ApplyQuickZoom` free function holding the store/restore arithmetic. `RunTick` in `src/main.cpp` rising-edge-detects the existing `inHeld`/`outHeld` flags, feeds the detector a QPC timestamp, and applies the result by snapping the level so the existing same-tick zoom-in/zoom-out transitions handle all rendering. Three hot-reloadable config knobs gate and tune it.

**Tech Stack:** C++17, MSVC `cl.exe`, doctest (`third_party/doctest.h`), Svelte/Vite config UI.

Spec: `docs/superpowers/specs/2026-06-03-quick-zoom-design.md`.

---

## File Structure

- `src/zoom_controller.h` (modify): add `ZoomController::setLevel`, the `QuickZoomDetector` class, the `QuickZoomResult` struct, and the `ApplyQuickZoom` free function.
- `src/zoom_controller.cpp` (modify): implementations of the above.
- `tests/test_quick_zoom.cpp` (create): unit tests for `QuickZoomDetector` and `ApplyQuickZoom` (auto-included by `tests\*.cpp`).
- `tests/test_zoom_controller.cpp` (modify): add `setLevel` clamp tests.
- `src/config.h` (modify): add `quickZoom`, `quickZoomWindowMs`, `quickZoomDefault`.
- `src/config.cpp` (modify): parse + clamp the three keys, add them to the default-ini template.
- `tests/test_config.cpp` (modify): parse + clamp tests for the three keys.
- `src/main.cpp` (modify): `TickState` fields + the edge-detect/apply block in `RunTick`.
- `ui/src/settings-schema.js` (modify): three rows in the Zoom section (no bridge/host change; `UpdateIniText` appends unknown keys, `setConfig` writes any key).

---

## Task 1: `ZoomController::setLevel` (instant snap)

**Files:**
- Modify: `src/zoom_controller.h` (inside `class ZoomController`, near `reset()`)
- Modify: `src/zoom_controller.cpp`
- Test: `tests/test_zoom_controller.cpp`

- [ ] **Step 1: Write the failing test**

Append to `tests/test_zoom_controller.cpp`:

```cpp
TEST_CASE("setLevel snaps and clamps to bounds") {
    ZoomController z(1.0, 8.0);
    z.setLevel(4.0);
    CHECK(z.level() == doctest::Approx(4.0));
    z.setLevel(100.0);                 // above max -> clamps to 8.0
    CHECK(z.level() == doctest::Approx(8.0));
    z.setLevel(0.5);                   // below min -> clamps to 1.0
    CHECK(z.level() == doctest::Approx(1.0));
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `build.bat test`
Expected: compile error - `setLevel` is not a member of `ZoomController`.

- [ ] **Step 3: Implement**

In `src/zoom_controller.h`, add inside `class ZoomController` right after the `reset();` declaration:

```cpp
    void setLevel(double l);       // instant snap to a level (clamped to [min,max]); dir_ untouched
```

In `src/zoom_controller.cpp`, add after `ZoomController::reset()`:

```cpp
void ZoomController::setLevel(double l) {
    level_ = std::min(maxLevel_, std::max(minLevel_, l));
}
```

(`<algorithm>` is already included in this file.)

- [ ] **Step 4: Run to verify it passes**

Run: `build.bat test`
Expected: all tests PASS (exit 0).

- [ ] **Step 5: Commit**

```bash
git add src/zoom_controller.h src/zoom_controller.cpp tests/test_zoom_controller.cpp
git commit -m "feat(zoom): ZoomController::setLevel instant snap"
```

---

## Task 2: `QuickZoomDetector` (double-tap detection)

**Files:**
- Modify: `src/zoom_controller.h` (after the `ZoomController` class)
- Modify: `src/zoom_controller.cpp`
- Test: `tests/test_quick_zoom.cpp` (create)

- [ ] **Step 1: Write the failing test**

Create `tests/test_quick_zoom.cpp`:

```cpp
#include "doctest.h"
#include "../src/zoom_controller.h"
using namespace wind;

TEST_CASE("double-tap inside the window fires once") {
    QuickZoomDetector d;
    d.setWindow(0.3);
    CHECK(d.update(true, false, 0.00) == false);   // first in-tap
    CHECK(d.update(true, false, 0.20) == true);    // second in-tap within 0.3s
    CHECK(d.update(false, false, 0.21) == false);  // no edge -> nothing
}
TEST_CASE("two taps outside the window do not fire") {
    QuickZoomDetector d;
    d.setWindow(0.3);
    CHECK(d.update(true, false, 0.00) == false);
    CHECK(d.update(true, false, 0.50) == false);   // 0.5s gap > window; just rearms
    CHECK(d.update(true, false, 0.60) == true);    // now within window of the 0.50 tap
}
TEST_CASE("channels are independent - in then out does not fire") {
    QuickZoomDetector d;
    d.setWindow(0.3);
    CHECK(d.update(true, false, 0.00) == false);   // in tap
    CHECK(d.update(false, true, 0.10) == false);   // out tap (different channel)
}
TEST_CASE("either channel double-tapped fires") {
    QuickZoomDetector d;
    d.setWindow(0.3);
    CHECK(d.update(false, true, 0.00) == false);
    CHECK(d.update(false, true, 0.15) == true);    // out double-tap
}
TEST_CASE("triple-tap = one fire then a fresh sequence") {
    QuickZoomDetector d;
    d.setWindow(0.3);
    CHECK(d.update(true, false, 0.00) == false);
    CHECK(d.update(true, false, 0.10) == true);    // tap 2 fires + consumes
    CHECK(d.update(true, false, 0.20) == false);   // tap 3 starts a new sequence
    CHECK(d.update(true, false, 0.30) == true);    // tap 4 fires
}
TEST_CASE("a changed window is respected") {
    QuickZoomDetector d;
    d.setWindow(0.1);
    CHECK(d.update(true, false, 0.00) == false);
    CHECK(d.update(true, false, 0.20) == false);   // 0.2s > 0.1s window -> rearm, no fire
}
TEST_CASE("reset clears pending taps") {
    QuickZoomDetector d;
    d.setWindow(0.3);
    CHECK(d.update(true, false, 0.00) == false);
    d.reset();
    CHECK(d.update(true, false, 0.10) == false);   // first tap was cleared
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `build.bat test`
Expected: compile error - `QuickZoomDetector` is undeclared.

- [ ] **Step 3: Implement**

In `src/zoom_controller.h`, add after the closing `};` of `class ZoomController` (still inside `namespace wind`):

```cpp
// Pure double-tap detector for quick zoom. Two independent channels (in, out): each remembers the
// time of its last down-edge; two down-edges of the SAME channel within the window fire once.
// A fire of either channel drives the same quick-zoom toggle (see ApplyQuickZoom). Fed rising edges
// from the tick loop with a monotonic timestamp (QPC seconds in the app; arbitrary in tests).
class QuickZoomDetector {
public:
    void setWindow(double seconds) { window_ = seconds; }
    // inEdge/outEdge: that channel went down THIS tick. nowSeconds: a monotonic clock. Returns true
    // exactly once when a double-tap completes (and consumes it, so a triple-tap restarts).
    bool update(bool inEdge, bool outEdge, double nowSeconds);
    void reset() { lastInDown_ = lastOutDown_ = kNever; }
private:
    static constexpr double kNever = -1e9;
    double window_      = 0.3;
    double lastInDown_  = kNever;
    double lastOutDown_ = kNever;
};
```

In `src/zoom_controller.cpp`, add after `ZoomController::setLevel`:

```cpp
bool QuickZoomDetector::update(bool inEdge, bool outEdge, double nowSeconds) {
    bool fire = false;
    if (inEdge) {
        if (nowSeconds - lastInDown_ <= window_) { fire = true; lastInDown_ = kNever; }
        else                                       lastInDown_ = nowSeconds;
    }
    if (outEdge) {
        if (nowSeconds - lastOutDown_ <= window_) { fire = true; lastOutDown_ = kNever; }
        else                                        lastOutDown_ = nowSeconds;
    }
    return fire;
}
```

- [ ] **Step 4: Run to verify it passes**

Run: `build.bat test`
Expected: all tests PASS (exit 0).

- [ ] **Step 5: Commit**

```bash
git add src/zoom_controller.h src/zoom_controller.cpp tests/test_quick_zoom.cpp
git commit -m "feat(zoom): QuickZoomDetector double-tap detection"
```

---

## Task 3: `ApplyQuickZoom` (store/restore arithmetic)

**Files:**
- Modify: `src/zoom_controller.h` (after `QuickZoomDetector`)
- Modify: `src/zoom_controller.cpp`
- Test: `tests/test_quick_zoom.cpp`

- [ ] **Step 1: Write the failing test**

Append to `tests/test_quick_zoom.cpp`:

```cpp
TEST_CASE("ApplyQuickZoom: zoomed above 200% snaps out and remembers") {
    QuickZoomResult r = ApplyQuickZoom(/*cur*/5.0, /*stored*/0.0, /*def*/4.0, /*max*/12.0);
    CHECK(r.newLevel  == doctest::Approx(1.0));
    CHECK(r.newStored == doctest::Approx(5.0));
}
TEST_CASE("ApplyQuickZoom: shallow zoom (<=200%) snaps out but is NOT remembered") {
    QuickZoomResult r = ApplyQuickZoom(/*cur*/1.5, /*stored*/5.0, /*def*/4.0, /*max*/12.0);
    CHECK(r.newLevel  == doctest::Approx(1.0));
    CHECK(r.newStored == doctest::Approx(5.0));   // prior memory preserved
}
TEST_CASE("ApplyQuickZoom: at 0% with a stored level snaps in to it (clamped to max)") {
    QuickZoomResult in = ApplyQuickZoom(/*cur*/1.0, /*stored*/5.0, /*def*/4.0, /*max*/12.0);
    CHECK(in.newLevel == doctest::Approx(5.0));
    QuickZoomResult clamped = ApplyQuickZoom(/*cur*/1.0, /*stored*/20.0, /*def*/4.0, /*max*/12.0);
    CHECK(clamped.newLevel == doctest::Approx(12.0));
}
TEST_CASE("ApplyQuickZoom: at 0% with nothing stored uses the default") {
    QuickZoomResult r = ApplyQuickZoom(/*cur*/1.0, /*stored*/0.0, /*def*/4.0, /*max*/12.0);
    CHECK(r.newLevel  == doctest::Approx(4.0));
    CHECK(r.newStored == doctest::Approx(0.0));    // memory unchanged on snap-in
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `build.bat test`
Expected: compile error - `ApplyQuickZoom` / `QuickZoomResult` undeclared.

- [ ] **Step 3: Implement**

In `src/zoom_controller.h`, add after the `QuickZoomDetector` class (inside `namespace wind`):

```cpp
// Result of one quick-zoom toggle: the level to snap to, and the (possibly updated) remembered level.
struct QuickZoomResult { double newLevel; double newStored; };
// Pure toggle arithmetic. cur = current level, stored = remembered level (0 = none yet), def = the
// configured default, maxLevel = ceiling. If zoomed (cur > 1.0): snap out to 1.0, remembering cur
// only when it is above 200% (cur > 2.0). If at 1.0: snap in to stored (or def if none), clamped to
// [1.0, maxLevel].
QuickZoomResult ApplyQuickZoom(double cur, double stored, double def, double maxLevel);
```

In `src/zoom_controller.cpp`, add after `QuickZoomDetector::update`:

```cpp
QuickZoomResult ApplyQuickZoom(double cur, double stored, double def, double maxLevel) {
    constexpr double kEps = 1e-6;
    constexpr double kStoreThreshold = 2.0;        // remember the level being left only if > 200%
    QuickZoomResult r{cur, stored};
    if (cur > 1.0 + kEps) {                         // zoomed -> snap out to 0%
        if (cur > kStoreThreshold) r.newStored = cur;
        r.newLevel = 1.0;
    } else {                                        // at 0% -> snap in
        double target = (stored > 0.0) ? stored : def;
        if (target > maxLevel) target = maxLevel;
        if (target < 1.0)      target = 1.0;
        r.newLevel = target;
    }
    return r;
}
```

- [ ] **Step 4: Run to verify it passes**

Run: `build.bat test`
Expected: all tests PASS (exit 0).

- [ ] **Step 5: Commit**

```bash
git add src/zoom_controller.h src/zoom_controller.cpp tests/test_quick_zoom.cpp
git commit -m "feat(zoom): ApplyQuickZoom store/restore arithmetic"
```

---

## Task 4: Config knobs (`quickZoom`, `quickZoomWindowMs`, `quickZoomDefault`)

**Files:**
- Modify: `src/config.h`
- Modify: `src/config.cpp` (parse, clamp, default-ini template)
- Test: `tests/test_config.cpp`

- [ ] **Step 1: Write the failing test**

Append to `tests/test_config.cpp` (uses `ParseConfig`, already exercised in that file):

```cpp
TEST_CASE("quick-zoom config parses and clamps") {
    Config def = ParseConfig("");
    CHECK(def.quickZoom == 1);
    CHECK(def.quickZoomWindowMs == 300);
    CHECK(def.quickZoomDefault == doctest::Approx(4.0));

    Config c = ParseConfig("quickZoom=0\nquickZoomWindowMs=250\nquickZoomDefault=6.0\n");
    CHECK(c.quickZoom == 0);
    CHECK(c.quickZoomWindowMs == 250);
    CHECK(c.quickZoomDefault == doctest::Approx(6.0));

    Config hi = ParseConfig("quickZoomWindowMs=99999\nquickZoomDefault=99\n");
    CHECK(hi.quickZoomWindowMs == 2000);                 // clamped to max
    CHECK(hi.quickZoomDefault == doctest::Approx(50.0)); // clamped to max
    Config lo = ParseConfig("quickZoomWindowMs=1\nquickZoomDefault=0.1\n");
    CHECK(lo.quickZoomWindowMs == 50);                   // clamped to min
    CHECK(lo.quickZoomDefault == doctest::Approx(1.0));  // clamped to min
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `build.bat test`
Expected: compile error - `quickZoom` / `quickZoomWindowMs` / `quickZoomDefault` are not members of `Config`.

- [ ] **Step 3: Implement**

In `src/config.h`, add inside `struct Config` right after the `onboarded` field (line ~100):

```cpp
    // Quick zoom: double-tap either zoom key to toggle between 1.0x ("0%") and a remembered level.
    int    quickZoom         = 1;     // 1 = enabled (default on), 0 = off
    int    quickZoomWindowMs = 300;   // max ms between the two taps to count as a double-tap
    double quickZoomDefault  = 4.0;   // level to snap to when nothing has been remembered yet
```

In `src/config.cpp`, add to the `ParseConfig` else-if chain after the `onboarded` line (line 61):

```cpp
            else if (key == "quickZoom")          c.quickZoom = std::stoi(val);
            else if (key == "quickZoomWindowMs")  c.quickZoomWindowMs = std::stoi(val);
            else if (key == "quickZoomDefault")   c.quickZoomDefault = std::stod(val);
```

In `src/config.cpp`, add to the clamp block (after the `c.brightness` clamp, line ~76, before `return c;`):

```cpp
    c.quickZoomWindowMs = (int)clampd(c.quickZoomWindowMs, 50.0, 2000.0);
    c.quickZoomDefault  = clampd(c.quickZoomDefault,        1.0, 50.0);
```

In `src/config.cpp`, in `LoadConfig`'s default-ini template, add after the `smoothZoomRamp=0.6\n` line (line ~125):

```cpp
               "; quickZoom: double-tap either zoom key to toggle between 0% and your last level\n"
               ";   (above 200%); 1=on, 0=off\n"
               "quickZoom=1\n"
               "; quickZoomWindowMs: max milliseconds between the two taps (50-2000)\n"
               "quickZoomWindowMs=300\n"
               "; quickZoomDefault: level to jump to before you've set one (e.g. 4.0 = 400%)\n"
               "quickZoomDefault=4.0\n"
```

- [ ] **Step 4: Run to verify it passes**

Run: `build.bat test`
Expected: all tests PASS (exit 0).

- [ ] **Step 5: Commit**

```bash
git add src/config.h src/config.cpp tests/test_config.cpp
git commit -m "feat(config): quickZoom, quickZoomWindowMs, quickZoomDefault knobs"
```

---

## Task 5: Wire quick zoom into `RunTick`

**Files:**
- Modify: `src/main.cpp` (`TickState` struct ~line 89-115, and `RunTick` ~line 250-258)

No unit test (this is the Win32 tick glue; the decision logic is already covered by Tasks 1-4). Verification is an app build plus a manual smoke test.

- [ ] **Step 1: Add `TickState` fields**

In `src/main.cpp`, in the `TickState` struct, add these members (next to `recenterKeyWasDown`, ~line 106):

```cpp
    double quickZoomStored    = 0.0;           // remembered quick-zoom level (0 = none yet); in-memory
    bool   prevInHeld         = false;         // for rising-edge detection of the zoom-in channel
    bool   prevOutHeld        = false;
    QuickZoomDetector quickZoom;               // pure double-tap detector
```

- [ ] **Step 2: Add the detect/apply block in `RunTick`**

In `src/main.cpp`, insert this block immediately BEFORE the line `double lvl = t.zoom.level();` (~line 258), after the recenter-key handling:

```cpp
    // Quick zoom: a double-tap of EITHER zoom channel toggles between 1.0x and a remembered level.
    // Rising-edge-detect the already-computed held flags, feed the pure detector a QPC timestamp,
    // and apply the toggle by snapping the level so the SAME-tick zoom-in/out transitions below
    // (which key off lvl vs prevLvl) handle all the overlay/cursor work. Window is applied live
    // (hot-reload), mirroring setProfile. The two-tap ramp is harmless: the snap overrides it.
    bool inEdge  = inHeld  && !t.prevInHeld;
    bool outEdge = outHeld && !t.prevOutHeld;
    t.prevInHeld = inHeld; t.prevOutHeld = outHeld;
    if (t.cfg.quickZoom) {
        t.quickZoom.setWindow(t.cfg.quickZoomWindowMs / 1000.0);
        double nowSec = double(now.QuadPart) / double(t.freq.QuadPart);
        if (t.quickZoom.update(inEdge, outEdge, nowSec)) {
            QuickZoomResult qr = ApplyQuickZoom(t.zoom.level(), t.quickZoomStored,
                                                t.cfg.quickZoomDefault, t.cfg.maxLevel);
            t.zoom.setLevel(qr.newLevel);
            t.quickZoomStored = qr.newStored;
        }
    }
```

(`now` and `t.freq` are the QPC values already read at the top of `RunTick`. `zoom_controller.h` is already included by `main.cpp`.)

- [ ] **Step 3: Build the app**

Run: `build.bat`
Expected: compiles and links, emits `Wind.exe`, exit 0. No new warnings.

- [ ] **Step 4: Run the full test suite (guard against regressions)**

Run: `build.bat test`
Expected: all tests PASS (exit 0).

- [ ] **Step 5: Manual smoke test**

Launch `Wind.exe` (a zoom key must be bound; bind one via the tray/config if needed). Verify:
- Manually hold-zoom in past 200%, then double-tap a zoom key -> snaps instantly to 0% (overlay hides, cursor restored).
- Double-tap again at 0% -> snaps back to the level you left.
- From a fresh launch at 0%, double-tap -> snaps to 400% (the default).
- Hold-zoom to under 200%, double-tap -> snaps to 0%; double-tap again -> returns to the earlier remembered level (the shallow zoom was not remembered).
- Normal hold-to-zoom still works unchanged.

Quit cleanly via the tray or Ctrl+Alt+Q (confirm the cursor is restored).

- [ ] **Step 6: Commit**

```bash
git add src/main.cpp
git commit -m "feat(zoom): wire quick-zoom double-tap toggle into the tick loop"
```

---

## Task 6: Config UI rows

**Files:**
- Modify: `ui/src/settings-schema.js` (the `zoom` section)

No bridge or host change: `getConfig`/`setConfig` round-trip arbitrary keys, and `UpdateIniText` appends keys the ini does not yet contain.

- [ ] **Step 1: Add the rows**

In `ui/src/settings-schema.js`, inside the `zoom` section `rows` array, add after the `smoothZoomRamp` row (line ~15):

```javascript
    { key:'quickZoom', type:'toggle', label:'Quick zoom (double-tap)', desc:'Double-tap a zoom key to toggle between 0% and your last level (above 200%).', def:1 },
    { key:'quickZoomWindowMs', type:'slider', label:'Double-tap window (ms)', desc:'Max time between the two taps.', min:150, max:600, step:25, def:300, dependsOn:'quickZoom' },
    { key:'quickZoomDefault', type:'slider', label:'Quick-zoom default', desc:'Level used before you set one (4 = 400%).', min:2, max:50, step:0.5, def:4.0, dependsOn:'quickZoom' },
```

- [ ] **Step 2: Build the config UI + host**

Run: `build.bat config`
Expected: npm build of `ui/` succeeds, `WindConfig.exe` compiles, exit 0.

- [ ] **Step 3: Manual verification**

Launch `WindConfig.exe`. In the Zoom section, confirm:
- "Quick zoom (double-tap)" toggle appears (on by default).
- The window and default sliders appear and grey out when the toggle is off (`dependsOn`).
- Changing a value and Applying writes it to `magnifier.ini` (open the ini to confirm the key), and the running `Wind.exe` picks it up within ~1s (hot-reload).

- [ ] **Step 4: Commit**

```bash
git add ui/src/settings-schema.js ui/dist
git commit -m "feat(ui): quick-zoom settings rows in the Zoom section"
```

---

## Final verification

- [ ] `build.bat test` -> all pure tests pass (exit 0).
- [ ] `build.bat` -> app builds clean.
- [ ] `build.bat config` -> config UI + host build clean.
- [ ] Manual smoke tests in Task 5 Step 5 and Task 6 Step 3 all pass.
- [ ] Open a GitHub issue for the feature, push the branch, open a PR referencing the issue (per project workflow; `quick-zoom` branch is already checked out).
