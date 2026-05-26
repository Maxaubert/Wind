# Auto-Match Cursor Sensitivity Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the magnifier pan at the same speed as the user's real Windows cursor (acceleration included) by reading the OS cursor's own per-tick motion while zoomed, falling back to raw mickeys only when a game locks the cursor.

**Architecture:** A new pure `LockDetector` (hysteresis) decides free-vs-locked from per-tick signals. `CursorMapper` integrates a resolved pixel delta (the sensitivity multiply leaves the pure mapper). `main.cpp` samples `GetCursorPos`/`GetClipCursor` each zoomed tick, computes the OS-cursor delta from where it last placed the cursor (free) or scales raw mickeys (locked), and feeds the chosen delta to the mapper. Both regimes integrate a delta into the same accumulator, so a regime switch never snaps position.

**Tech Stack:** C++17, MSVC, Win32 (`GetCursorPos`/`SetCursorPos`/`GetClipCursor`), doctest. Spec: `docs/superpowers/specs/2026-05-26-auto-cursor-sensitivity-design.md`. Branch `feat/auto-sensitivity`, issue #38.

**Key commands:**
- Build + run unit tests: `build.bat test` (exit 0 = pass).
- App build: `build.bat` (emits `Wind.exe`; exit 0 = success).

**Conventions:** No em-dashes. Commit trailer on every commit:
```
Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
```

---

## File Structure

- **New** `src/lock_detector.{h,cpp}` - pure free/locked hysteresis state machine.
- **New** `tests/test_lock_detector.cpp` - its unit tests.
- **Modify** `src/cursor_mapper.{h,cpp}` - integrate a resolved pixel delta; drop the `sensitivity` ctor param.
- **Modify** `tests/test_cursor_mapper.cpp` - update for the new ctor/delta meaning.
- **Modify** `src/main.cpp` - oracle wiring (the feature).
- **Modify** `src/config.{h,cpp}` - repurpose the `cursorSensitivity` comments.
- **Modify** `build.bat` - add `src\lock_detector.cpp` to the test build.
- **Modify** `CLAUDE.md` - document the oracle + lock-fallback model.

---

## Task 1: `CursorMapper` integrates a pixel delta (drop `sensitivity`)

**Files:** Modify `src/cursor_mapper.h`, `src/cursor_mapper.cpp`, `tests/test_cursor_mapper.cpp`, and the 3 ctor call sites in `src/main.cpp` (so the app keeps compiling).

- [ ] **Step 1: Update the failing tests in `tests/test_cursor_mapper.cpp`**

Replace the whole file with (every `CursorMapper(...)` drops the sensitivity arg; the sensitivity test becomes a delta-integration test):
```cpp
#include "doctest.h"
#include "../src/cursor_mapper.h"

using wind::CursorMapper;

TEST_CASE("centered: cursor sits at screen center, no movement") {
    CursorMapper m(1920, 1080);
    m.reset(960, 540);
    auto r = m.update(0, 0, 2.0);
    CHECK(r.srcLeft == doctest::Approx(480.0));   // 960 - (1920/2)/2
    CHECK(r.srcTop  == doctest::Approx(270.0));
    CHECK(r.cursorScreenX == doctest::Approx(960.0));  // dead center
    CHECK(r.cursorScreenY == doctest::Approx(540.0));
    CHECK(r.clickDesktopX == 960);
    CHECK(r.clickDesktopY == 540);
}

TEST_CASE("a pixel delta pans the world at desktop speed, cursor stays centered") {
    CursorMapper m(1920, 1080);
    m.reset(960, 540);
    auto r = m.update(20, 0, 2.0);       // +20 desktop px (zoom-independent)
    CHECK(m.centerX() == doctest::Approx(980.0));
    CHECK(r.srcLeft == doctest::Approx(500.0));        // 980 - 480
    CHECK(r.cursorScreenX == doctest::Approx(960.0));  // still centered
    CHECK(r.clickDesktopX == 980);                     // click point tracks lens center
}

TEST_CASE("edge: cursor shifts off-center when the view clamps at the desktop edge") {
    CursorMapper m(1920, 1080);
    m.reset(10, 540);                    // near the left edge
    auto r = m.update(0, 0, 4.0);        // viewW = 480, srcLeft clamps to 0
    CHECK(r.srcLeft == doctest::Approx(0.0));
    CHECK(r.cursorScreenX == doctest::Approx(40.0));   // (10 - 0) * 4
    CHECK(r.clickDesktopX == 10);
}

TEST_CASE("lens center clamps to the desktop bounds") {
    CursorMapper m(1920, 1080);
    m.reset(0, 0);
    m.update(-500, -500, 2.0);           // pushes past the top-left
    CHECK(m.centerX() == doctest::Approx(0.0));
    CHECK(m.centerY() == doctest::Approx(0.0));
    m.reset(1920, 1080);
    m.update(500, 500, 2.0);             // pushes past the bottom-right
    CHECK(m.centerX() == doctest::Approx(1920.0));
    CHECK(m.centerY() == doctest::Approx(1080.0));
}

TEST_CASE("update integrates the pixel delta directly; zoom level does NOT change desktop speed") {
    CursorMapper m(1920, 1080);
    m.reset(960, 540);
    m.update(40, 0, 2.0);                // delta +40 -> center 1000 (no sensitivity scaling)
    CHECK(m.centerX() == doctest::Approx(1000.0));
    // Same delta at a higher zoom moves the lens the SAME desktop amount (world just scrolls
    // faster on screen) - zoom-independent.
    CursorMapper a(1920, 1080);  a.reset(960, 540);  a.update(40, 0, 2.0);
    CursorMapper b(1920, 1080);  b.reset(960, 540);  b.update(40, 0, 8.0);
    CHECK(a.centerX() == doctest::Approx(1000.0));
    CHECK(b.centerX() == doctest::Approx(1000.0));
}

TEST_CASE("smoothing eases the rendered center toward the target (light inertia)") {
    CursorMapper m(1920, 1080, 0.5);    // alpha = 0.5
    m.reset(960, 540);
    m.update(40, 0, 2.0);    // target 1000; rendered = 960 + (1000-960)*0.5 = 980
    CHECK(m.centerX() == doctest::Approx(980.0));
    m.update(0, 0, 2.0);     // target still 1000; rendered = 980 + (1000-980)*0.5 = 990
    CHECK(m.centerX() == doctest::Approx(990.0));
}

TEST_CASE("smoothing 0 snaps instantly (no inertia)") {
    CursorMapper m(1920, 1080, 0.0);
    m.reset(960, 540);
    m.update(40, 0, 2.0);
    CHECK(m.centerX() == doctest::Approx(1000.0));   // straight to target
}

TEST_CASE("reset overrides the accumulated center") {
    CursorMapper m(1920, 1080);
    m.reset(100, 100);
    m.update(50, 50, 2.0);
    m.reset(800, 400);
    CHECK(m.centerX() == doctest::Approx(800.0));
    CHECK(m.centerY() == doctest::Approx(400.0));
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `build.bat test`
Expected: compile error - `CursorMapper(1920, 1080)` and `(1920,1080,0.5)` do not match the current `CursorMapper(int, int, double sensitivity, double smoothing=0.0)` ctor (the 2-arg form is missing; the 3-arg form binds 0.5 to `sensitivity`, but the delta-integration assertions like center 1000 from `update(40,...)` would fail since the current code multiplies by sensitivity). Fail confirms the change is needed.

- [ ] **Step 3: Change the `CursorMapper` header**

In `src/cursor_mapper.h`, replace:
```cpp
    CursorMapper(int screenW, int screenH, double sensitivity, double smoothing = 0.0);
    void reset(double centerX, double centerY);    // pin both target + rendered center
    MapResult update(int rawDx, int rawDy, double level);
```
with:
```cpp
    CursorMapper(int screenW, int screenH, double smoothing = 0.0);
    void reset(double centerX, double centerY);    // pin both target + rendered center
    // dx/dy: the pixel delta to apply to the lens center this tick (already resolved by the
    // caller - the OS cursor's own motion when free, or scaled raw input when a game locks it).
    MapResult update(int dx, int dy, double level);
```
and remove the `double sens_;` member line (leaving `double alpha_;` and the rest):
```cpp
    int sw_, sh_;
    double sens_;
    double alpha_;          // per-frame easing factor (1 - smoothing), clamped
```
becomes:
```cpp
    int sw_, sh_;
    double alpha_;          // per-frame easing factor (1 - smoothing), clamped
```
Also update the class doc comment line `// Integrates raw-input deltas into a float lens center` to `// Integrates per-tick pixel deltas into a float lens center`.

- [ ] **Step 4: Change `CursorMapper` implementation**

In `src/cursor_mapper.cpp`, replace the constructor:
```cpp
CursorMapper::CursorMapper(int screenW, int screenH, double sensitivity, double smoothing)
    : sw_(screenW), sh_(screenH), sens_(sensitivity),
      cx_(screenW / 2.0), cy_(screenH / 2.0), tx_(screenW / 2.0), ty_(screenH / 2.0) {
    alpha_ = 1.0 - smoothing;
    if (alpha_ > 1.0) alpha_ = 1.0;
    if (alpha_ < 0.05) alpha_ = 0.05;     // never fully stall (keep responsiveness)
}
```
with:
```cpp
CursorMapper::CursorMapper(int screenW, int screenH, double smoothing)
    : sw_(screenW), sh_(screenH),
      cx_(screenW / 2.0), cy_(screenH / 2.0), tx_(screenW / 2.0), ty_(screenH / 2.0) {
    alpha_ = 1.0 - smoothing;
    if (alpha_ > 1.0) alpha_ = 1.0;
    if (alpha_ < 0.05) alpha_ = 0.05;     // never fully stall (keep responsiveness)
}
```
Then in `update`, replace the signature and the integration lines:
```cpp
MapResult CursorMapper::update(int rawDx, int rawDy, double level) {
    if (level < 1.0) level = 1.0;
    // Target moves at *desktop* speed (not divided by zoom): the focus reaches things at the
    // same hand-speed whether at 2x or 8x, matching Windows Magnifier. Tune with sensitivity.
    tx_ += rawDx * sens_;
    ty_ += rawDy * sens_;
```
with:
```cpp
MapResult CursorMapper::update(int dx, int dy, double level) {
    if (level < 1.0) level = 1.0;
    // Apply the caller-resolved pixel delta at *desktop* speed (not divided by zoom): the focus
    // reaches things at the same hand-speed whether at 2x or 8x, matching Windows Magnifier.
    tx_ += dx;
    ty_ += dy;
```
(Leave the rest of `update` - the clamps, easing, and `MapResult` construction - unchanged.)

- [ ] **Step 5: Fix the 3 `CursorMapper` ctor call sites in `main.cpp`**

In `src/main.cpp`, replace each of these three lines (they currently pass `cursorSensitivity`):

In the `TickState` constructor:
```cpp
          mapper(m.w, m.h, c.cursorSensitivity, c.cursorSmoothing) {}
```
->
```cpp
          mapper(m.w, m.h, c.cursorSmoothing) {}
```

In `RunTick`'s config hot-reload block:
```cpp
            t.mapper = CursorMapper(t.mon.w, t.mon.h, nc.cursorSensitivity, nc.cursorSmoothing);
```
->
```cpp
            t.mapper = CursorMapper(t.mon.w, t.mon.h, nc.cursorSmoothing);
```

In `RunTick`'s monitor-retarget block:
```cpp
                    t.mapper = CursorMapper(nt.w, nt.h, t.cfg.cursorSensitivity, t.cfg.cursorSmoothing);
```
->
```cpp
                    t.mapper = CursorMapper(nt.w, nt.h, t.cfg.cursorSmoothing);
```

(After this task the app still behaves as before for the default config: `main` still passes raw `dx,dy` to `mapper.update`, which now integrates them 1:1 - identical to the old default `sensitivity=1.0`. The oracle + locked scaling arrive in Task 3.)

- [ ] **Step 6: Run unit tests + app build**

Run: `build.bat test` then `build.bat`
Expected: both exit 0. Tests pass with the new ctor/delta semantics; app compiles (all 3 ctor sites updated).

- [ ] **Step 7: Commit**

```bash
git add src/cursor_mapper.h src/cursor_mapper.cpp tests/test_cursor_mapper.cpp src/main.cpp
git commit -m "refactor: CursorMapper integrates a resolved pixel delta (drop sensitivity param) (#38)

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

## Task 2: `LockDetector` pure unit (TDD)

**Files:** Create `src/lock_detector.h`, `src/lock_detector.cpp`, `tests/test_lock_detector.cpp`; modify `build.bat`.

- [ ] **Step 1: Add `src/lock_detector.cpp` to the test build**

In `build.bat`, in the `:test` section, find the source list line:
```
   src\transform.cpp src\zoom_controller.cpp src\config.cpp src\cursor_mapper.cpp ^
```
and change it to add `lock_detector.cpp`:
```
   src\transform.cpp src\zoom_controller.cpp src\config.cpp src\cursor_mapper.cpp src\lock_detector.cpp ^
```

- [ ] **Step 2: Write the failing tests**

Create `tests/test_lock_detector.cpp`:
```cpp
#include "doctest.h"
#include "../src/lock_detector.h"

using wind::LockDetector;

// Mirrors the constants in lock_detector.cpp: kLockTicks=6, kFreeTicks=3, kRawActive=4, kCursorMoved=1.

TEST_CASE("starts free") {
    LockDetector d;
    CHECK(!d.locked());
}

TEST_CASE("a confined clip rect locks immediately") {
    LockDetector d;
    CHECK(d.update(/*clipConfined=*/true, 0, 0));
    CHECK(d.locked());
}

TEST_CASE("raw active + cursor frozen for kLockTicks locks (hysteresis: not before)") {
    LockDetector d;
    for (int i = 0; i < 5; ++i) CHECK(!d.update(false, 10, 0));   // 5 < kLockTicks
    CHECK(d.update(false, 10, 0));                                // 6th -> locked
    CHECK(d.locked());
}

TEST_CASE("a single frozen tick does not lock") {
    LockDetector d;
    d.update(false, 10, 0);
    CHECK(!d.locked());
}

TEST_CASE("once locked, cursor tracking input for kFreeTicks unlocks (not before)") {
    LockDetector d;
    for (int i = 0; i < 6; ++i) d.update(false, 10, 0);   // -> locked
    REQUIRE(d.locked());
    CHECK(d.update(false, 10, 5));   // moving, streak 1
    CHECK(d.update(false, 10, 5));   // streak 2 -> still locked
    CHECK(d.locked());
    CHECK(!d.update(false, 10, 5));  // streak 3 == kFreeTicks -> free
    CHECK(!d.locked());
}

TEST_CASE("idle ticks hold the current state") {
    LockDetector d;
    for (int i = 0; i < 6; ++i) d.update(false, 10, 0);   // locked
    REQUIRE(d.locked());
    d.update(false, 0, 0);           // idle: no raw, no cursor move
    CHECK(d.locked());               // still locked
}

TEST_CASE("slow desktop motion (cursor moves occasionally) never locks") {
    LockDetector d;
    // raw active every tick, but the cursor moves >=1px every other tick (accel sub-pixel
    // accumulating) - the moving ticks reset the lock streak, so it never reaches kLockTicks.
    for (int i = 0; i < 30; ++i) {
        int curMag = (i % 2 == 0) ? 0 : 2;
        d.update(false, 6, curMag);
        CHECK(!d.locked());
    }
}

TEST_CASE("reset returns to free") {
    LockDetector d;
    d.update(true, 0, 0);
    REQUIRE(d.locked());
    d.reset();
    CHECK(!d.locked());
}
```

- [ ] **Step 3: Run tests to verify they fail**

Run: `build.bat test`
Expected: compile error - `lock_detector.h` does not exist yet / `wind::LockDetector` undefined.

- [ ] **Step 4: Create `src/lock_detector.h`**

```cpp
#pragma once
namespace wind {
// Decides whether the OS cursor is "locked" by a game (so the magnifier must pan from raw mouse
// input rather than the OS cursor's own motion). Pure, with hysteresis so a single contrary tick
// never flips the state - panning never flickers. Fed per-tick Win32 signals by main.cpp.
class LockDetector {
public:
    // clipConfined: a smaller-than-virtual-desktop ClipCursor rect is active (direct lock signal).
    // rawMag    : |rawDx| + |rawDy| this tick (mouse motion at the HID level).
    // cursorMag : |cursorDx| + |cursorDy| this tick (how far the OS cursor actually moved).
    // Returns the (possibly updated) locked state.
    bool update(bool clipConfined, int rawMag, int cursorMag);
    bool locked() const { return locked_; }
    void reset();   // back to free (call on zoom-in / recenter / monitor retarget)
private:
    bool locked_ = false;
    int  lockStreak_ = 0;   // consecutive ticks of (raw active, OS cursor frozen)
    int  freeStreak_ = 0;   // consecutive ticks of (OS cursor moving with input)
};
}
```

- [ ] **Step 5: Create `src/lock_detector.cpp`**

```cpp
#include "lock_detector.h"
namespace wind {
namespace {
constexpr int kRawActive  = 4;   // raw magnitude that counts as deliberate mouse motion
constexpr int kCursorMoved = 1;  // OS cursor moved at least this many px (it tracked input)
constexpr int kLockTicks  = 6;   // consecutive raw-active + cursor-frozen ticks -> lock
constexpr int kFreeTicks  = 3;   // consecutive cursor-moving ticks -> unlock
}

void LockDetector::reset() { locked_ = false; lockStreak_ = 0; freeStreak_ = 0; }

bool LockDetector::update(bool clipConfined, int rawMag, int cursorMag) {
    // Direct, reliable signal: a confined clip rect means a game has clipped the cursor.
    if (clipConfined) { locked_ = true; lockStreak_ = 0; freeStreak_ = 0; return locked_; }

    if (cursorMag >= kCursorMoved) {
        // The OS cursor is tracking input -> evidence of free movement.
        freeStreak_++; lockStreak_ = 0;
        if (freeStreak_ >= kFreeTicks) locked_ = false;
    } else if (rawMag >= kRawActive) {
        // Mouse moving but OS cursor frozen -> evidence of a lock.
        lockStreak_++; freeStreak_ = 0;
        if (lockStreak_ >= kLockTicks) locked_ = true;
    } else {
        // Idle (no significant input, cursor still): neither streak grows; hold current state.
        lockStreak_ = 0; freeStreak_ = 0;
    }
    return locked_;
}
}
```

- [ ] **Step 6: Run tests to verify they pass**

Run: `build.bat test`
Expected: exit 0, all `LockDetector` cases pass (plus the existing suites).

- [ ] **Step 7: Commit**

```bash
git add src/lock_detector.h src/lock_detector.cpp tests/test_lock_detector.cpp build.bat
git commit -m "feat: LockDetector pure free/locked hysteresis unit (#38)

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

## Task 3: Oracle wiring in `main.cpp` (activates the feature)

**Files:** Modify `src/main.cpp`.

- [ ] **Step 1: Include the detector and the math headers**

In `src/main.cpp`, after the existing `#include "tray.h"` line (the last project include near the top), add:
```cpp
#include "lock_detector.h"
```
And in the system-include block near the top (which has `#include <climits>` etc.), add:
```cpp
#include <cmath>
#include <cstdlib>
```

- [ ] **Step 2: Add the oracle state to `TickState`**

In `struct TickState`, add these two fields right after the `CursorMapper   mapper;` line:
```cpp
    LockDetector   detector;    // free vs game-locked cursor
    POINT          lastSetVirtual{};  // where we last SetCursorPos'd (virtual px); for the OS-cursor delta
```

- [ ] **Step 3: Rename the drained raw deltas in `RunTick`**

In `RunTick`, replace:
```cpp
    int dx, dy; g_input.drainRaw(dx, dy);
```
with:
```cpp
    int rawDx, rawDy; g_input.drainRaw(rawDx, rawDy);
```

- [ ] **Step 4: Initialize the oracle baseline on zoom-in and recenter**

In `RunTick`'s zoom-in block, replace:
```cpp
            POINT pt; GetCursorPos(&pt);
            t.mapper.reset(pt.x - t.mon.x, pt.y - t.mon.y);   // virtual -> local monitor coords
            t.renderEngine.hideSystemCursor(true);
            t.renderEngine.invalidateCapture();       // grab a live frame, not a stale cached one
```
with:
```cpp
            POINT pt; GetCursorPos(&pt);
            t.mapper.reset(pt.x - t.mon.x, pt.y - t.mon.y);   // virtual -> local monitor coords
            t.lastSetVirtual = pt;        // baseline for the OS-cursor delta (first delta = 0)
            t.detector.reset();           // start free
            t.renderEngine.hideSystemCursor(true);
            t.renderEngine.invalidateCapture();       // grab a live frame, not a stale cached one
```
And replace the recenter line:
```cpp
        if (recenter) { POINT pt; GetCursorPos(&pt); t.mapper.reset(pt.x - t.mon.x, pt.y - t.mon.y); }
```
with:
```cpp
        if (recenter) { POINT pt; GetCursorPos(&pt); t.mapper.reset(pt.x - t.mon.x, pt.y - t.mon.y); t.lastSetVirtual = pt; }
```

- [ ] **Step 5: Resolve the pan delta (oracle when free, raw when locked)**

In `RunTick`, replace this single line:
```cpp
        MapResult r = t.mapper.update(dx, dy, lvl);
```
with:
```cpp
        // Resolve the pan delta. FREE: the OS cursor's own motion since we last placed it - Windows'
        // pointer acceleration is already applied, so the magnifier matches the real cursor. LOCKED:
        // a game has the cursor clipped/recentered, so pan from raw mickeys scaled by cursorSensitivity
        // (acceleration doesn't apply to relative-mouse game input).
        POINT cur; GetCursorPos(&cur);
        int curDx = cur.x - t.lastSetVirtual.x;
        int curDy = cur.y - t.lastSetVirtual.y;
        RECT clip{}; GetClipCursor(&clip);
        int vsx = GetSystemMetrics(SM_XVIRTUALSCREEN), vsy = GetSystemMetrics(SM_YVIRTUALSCREEN);
        int vsw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vsh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        bool clipConfined = clip.left > vsx || clip.top > vsy ||
                            clip.right < vsx + vsw || clip.bottom < vsy + vsh;
        bool locked = t.detector.update(clipConfined,
                                        std::abs(rawDx) + std::abs(rawDy),
                                        std::abs(curDx) + std::abs(curDy));
        int dx, dy;
        if (locked) {
            dx = (int)std::lround(rawDx * t.cfg.cursorSensitivity);
            dy = (int)std::lround(rawDy * t.cfg.cursorSensitivity);
        } else {
            dx = curDx; dy = curDy;
        }
        // Defensive: bound one tick's pan to the monitor span so a stray cursor jump (e.g. the OS
        // cursor briefly escaping to another monitor) cannot teleport the lens. cx_ also clamps.
        if (dx >  t.mon.w) dx =  t.mon.w; else if (dx < -t.mon.w) dx = -t.mon.w;
        if (dy >  t.mon.h) dy =  t.mon.h; else if (dy < -t.mon.h) dy = -t.mon.h;
        MapResult r = t.mapper.update(dx, dy, lvl);
```

- [ ] **Step 6: Record where we placed the cursor, for next tick's delta**

In `RunTick`, the existing reveal line is:
```cpp
        if (zoomIn) t.renderEngine.setVisible(true);
```
Immediately AFTER `t.renderEngine.renderFrame(p);` (which is a few lines above that, and which does the `SetCursorPos(clickDesktop+origin)` internally), and BEFORE the `if (zoomIn) t.renderEngine.setVisible(true);` line, add:
```cpp
        // renderFrame SetCursorPos'd the OS cursor to clickDesktop+origin; remember it so next tick's
        // GetCursorPos delta measures only the user's hand motion since.
        t.lastSetVirtual.x = r.clickDesktopX + t.mon.x;
        t.lastSetVirtual.y = r.clickDesktopY + t.mon.y;
```
So that region reads, in order: `... FillRenderParams(...); t.renderEngine.renderFrame(p); [the two lastSetVirtual lines]; if (zoomIn) t.renderEngine.setVisible(true);`.

- [ ] **Step 7: App build + unit tests**

Run: `build.bat` then `build.bat test`
Expected: both exit 0. (`std::abs`/`std::lround` from `<cstdlib>`/`<cmath>`; `POINT`/`RECT`/`GetClipCursor`/`SM_*VIRTUALSCREEN` from `<windows.h>`, already included.)

- [ ] **Step 8: Commit**

```bash
git add src/main.cpp
git commit -m "feat: pan from the OS cursor's own motion (auto-match accel); raw only when locked (#38)

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

## Task 4: Config + docs

**Files:** Modify `src/config.h`, `src/config.cpp`, `CLAUDE.md`.

- [ ] **Step 1: Repurpose the `cursorSensitivity` comment in `config.h`**

In `src/config.h`, replace:
```cpp
    double cursorSensitivity = 1.0;  // lens pan speed per raw count
```
with:
```cpp
    // Raw-input pan scale used ONLY while a game has locked the cursor (relative-mouse mode).
    // Free desktop panning auto-matches the OS cursor (acceleration included) and ignores this.
    double cursorSensitivity = 1.0;
```

- [ ] **Step 2: Repurpose the default-ini comment in `config.cpp`**

In `src/config.cpp`, in `LoadConfig`'s default-ini text, replace:
```cpp
               "; cursorSensitivity: pan speed per raw count\n"
               "cursorSensitivity=1.0\n"
```
with:
```cpp
               "; cursorSensitivity: raw-pan scale while a GAME locks the cursor; free desktop\n"
               ";   panning auto-matches the OS cursor (incl. acceleration) and ignores this\n"
               "cursorSensitivity=1.0\n"
```

- [ ] **Step 3: Document the model in `CLAUDE.md`**

In `CLAUDE.md`, in the `## IMPORTANT gotchas` section, add this bullet at the end of the list:
```markdown
- CURSOR SENSITIVITY auto-matches the real OS cursor: while zoomed (cursor hidden), each tick reads
  the OS cursor's own movement since our last `SetCursorPos` (Windows' pointer acceleration already
  applied) and pans by that - so panning equals the user's normal cursor without reimplementing
  ballistics. `GetCursorPos` is usable as this "oracle" only because we read it BEFORE re-setting it
  each tick. Raw mickeys are kept solely to (a) feed `LockDetector` (a game clipping/recentering the
  cursor -> `GetClipCursor` confined, or raw-active-but-cursor-frozen with hysteresis) and (b) drive
  panning while locked (scaled by `cursorSensitivity`). Both regimes integrate a DELTA into the same
  accumulator, so a free/locked switch never snaps position - this is why it avoids the old Tracker
  flicker (issue #3). Do not "simplify" back to a fixed sensitivity multiplier.
```

- [ ] **Step 4: Build (sanity)**

Run: `build.bat` then `build.bat test`
Expected: both exit 0 (comment/doc-only changes).

- [ ] **Step 5: Commit**

```bash
git add src/config.h src/config.cpp CLAUDE.md
git commit -m "docs: repurpose cursorSensitivity (locked-only) + CLAUDE.md oracle model (#38)

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

## Task 5: Final gate + push + PR

**Files:** none (git/GitHub only). The manual single-monitor verification (the acceleration-match feel + game-lock fallback) is performed by the user before the PR opens.

- [ ] **Step 1: Final build + test gate**

Run: `build.bat` and `build.bat test`
Expected: both exit 0; all unit suites pass (incl. the new `LockDetector` cases and the updated `CursorMapper` cases).

- [ ] **Step 2: Push the branch**

```bash
git push -u origin feat/auto-sensitivity
```

- [ ] **Step 3: Open the PR (references issue #38)**

```bash
gh pr create --base feat/own-renderer --head feat/auto-sensitivity \
  --title "Auto-match cursor sensitivity to the real OS cursor (#38)" \
  --body "Pan the lens by the OS cursor's own per-tick motion (Windows acceleration already applied) so the magnifier matches the user's real cursor, instead of a fixed cursorSensitivity multiplier. Closes #38.

- New pure LockDetector (GetClipCursor + raw-active/cursor-frozen hysteresis); both free and locked regimes integrate a DELTA into the shared accumulator, so a regime switch never snaps position (avoids the old Tracker flicker, issue #3).
- CursorMapper integrates a resolved pixel delta (drops the sensitivity ctor param).
- main.cpp reads GetCursorPos BEFORE re-setting it each tick to measure the OS cursor's motion (the oracle); falls back to raw mickeys scaled by cursorSensitivity only while a game locks the cursor.
- cursorSensitivity repurposed: locked-mode raw scale only; free desktop panning ignores it.

Root cause: reporter's pointer slider is neutral but mouse acceleration is ON, so a flat raw*1.0 mismatched the accel'd cursor.

Verification: new/updated unit tests for LockDetector + CursorMapper; clean /W4 build + build.bat test. User-verified live: slow + fast desktop pans now match the real cursor; a cursor-locking game still pans via raw; clean zoom/recenter. Confirmed the live assumption that per-frame SetCursorPos doesn't perturb Windows' velocity-based acceleration.

Spec: docs/superpowers/specs/2026-05-26-auto-cursor-sensitivity-design.md
Plan: docs/superpowers/plans/2026-05-26-auto-cursor-sensitivity.md

🤖 Generated with [Claude Code](https://claude.com/claude-code)"
```

- [ ] **Step 4: Report the PR URL to the user.**

---

## Self-Review (completed during planning)

**Spec coverage:**
- Oracle per-tick flow (GetCursorPos delta, GetClipCursor, detector, resolve, SetCursorPos, lastSetVirtual) -> Task 3. ✓
- `LockDetector` (clip signal + hysteresis heuristic, reset) -> Task 2. ✓
- `CursorMapper` integrates resolved delta, drops `sensitivity` -> Task 1. ✓
- `cursorSensitivity` repurposed (locked-only) -> Task 4 (comments) + Task 3 (used only in the locked branch). ✓
- Init `lastSetVirtual` + `detector.reset()` on zoom-in/recenter -> Task 3 Step 4. ✓
- Per-tick delta clamp -> Task 3 Step 5. ✓
- Tests for both pure units -> Task 1 (mapper) + Task 2 (detector). ✓
- `build.bat` test-build adds `lock_detector.cpp` -> Task 2 Step 1. ✓
- CLAUDE.md note -> Task 4 Step 3. ✓
- issue->branch->PR, gates -> Task 5. ✓

**Placeholder scan:** none. Every code step shows full code; the `LockDetector` constants are concrete (4/1/6/3); the tests use literal tick counts matching them.

**Type consistency:** `CursorMapper(int,int,double smoothing=0.0)` and `update(int dx,int dy,double level)` consistent across Task 1 (def), the test file, and Task 1 Step 5 + Task 3 (call sites). `LockDetector::update(bool,int,int)` / `locked()` / `reset()` consistent across Task 2 def, its tests, and Task 3 usage. `TickState.lastSetVirtual` (POINT) and `TickState.detector` used consistently in Task 3. `cursorSensitivity` referenced only in the locked branch (Task 3 Step 5) + comments (Task 4). The monitor-retarget rebuild (Task 1 Step 5) and config-reload rebuild both drop the sensitivity arg, matching the new ctor. ✓
