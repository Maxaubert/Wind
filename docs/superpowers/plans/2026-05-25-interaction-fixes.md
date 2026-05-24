# Wind Interaction-Bug Fixes Implementation Plan

> **For agentic workers:** TDD, bite-sized steps, frequent commits. Steps use checkbox
> (`- [ ]`) syntax for tracking.

**Goal:** Kill the magnified-view flicker / mis-click bugs on the desktop (Issues 2 and
3 in `docs/KNOWN-ISSUES.md`) and make the zoom side-buttons work over elevated windows
(Issue 1), without regressing the core game cursor-lock feature.

**Architecture:** Two changes.
1. **Tracker lock detector (pure logic, fully unit-tested).** Replace the single-tick
   free/locked heuristic with a hysteresis detector: only treat the cursor as locked
   after `kLockEngageTicks` consecutive frozen-cursor-with-raw ticks, and hold (never
   jump) the lens while unconfirmed. This removes the off-centre/recenter oscillation
   (flicker, Issue 3) and the resulting cursor-vs-view divergence that broke clicking and
   the I-beam (Issue 2), while preserving lens-follows-locked-cursor for games.
2. **Raw-Input side-button detection (Win32).** Read XBUTTON state from the existing
   `WM_INPUT` stream in addition to the `WH_MOUSE_LL` hook. Raw Input is delivered even
   when an elevated window is foreground, where a medium-IL low-level hook is not invoked
   (UIPI). Additive and idempotent with the hook.

**Tech Stack:** C++17, MSVC, doctest. Pure logic in `src/tracker.*`; Win32 in
`src/main.cpp`. Build/test via `build.bat` / `build.bat test`.

---

## File Structure

- `src/tracker.h` - add `kLockEngageTicks`, `locked()` accessor, lock state fields.
- `src/tracker.cpp` - rewrite `update()` with the hysteresis lock detector.
- `tests/test_tracker.cpp` - rewrite locked-mode tests (they currently encode the
  buggy single-tick lock), add flicker-regression tests.
- `src/main.cpp` - decode XBUTTON from `WM_INPUT`; set zoom-button ids from config;
  add a locked-tick counter to the diagnostics line for real-world verification.

No new files. No changes to `input_router.*` (the hook keeps swallowing on normal
windows; raw input only adds a second, elevation-proof state source).

---

## Task 1: Tracker hysteresis lock detector (Issues 2 + 3)

**Files:**
- Modify: `src/tracker.h`
- Modify: `src/tracker.cpp`
- Test: `tests/test_tracker.cpp`

- [ ] **Step 1: Replace the tracker tests with corrected behavior (write failing tests first)**

Overwrite `tests/test_tracker.cpp` with:

```cpp
#include "doctest.h"
#include "../src/tracker.h"
using namespace wind;

// --- Free mode (normal desktop) ------------------------------------------------

TEST_CASE("free mode follows the OS cursor when it moves") {
    Tracker t(1920, 1080, 1.0);
    t.update(100, 100, 0, 0);
    CHECK(t.centerX() == doctest::Approx(100));
    CHECK(t.centerY() == doctest::Approx(100));
    t.update(300, 200, 0, 0);
    CHECK(t.centerX() == doctest::Approx(300));
    CHECK(t.centerY() == doctest::Approx(200));
    CHECK_FALSE(t.locked());
}

TEST_CASE("holds still when neither cursor nor raw move") {
    Tracker t(1920, 1080, 1.0);
    t.update(700, 700, 0, 0);
    t.update(700, 700, 0, 0);
    CHECK(t.centerX() == doctest::Approx(700));
}

// --- Flicker regression (the reported bug) -------------------------------------

TEST_CASE("a lone frozen-cursor tick with raw movement does NOT jump the lens") {
    // During free movement an occasional tick samples an unchanged GetCursorPos while
    // raw deltas arrive (sampling alias). Integrating it would jump the lens off-centre
    // then snap back next tick = the flicker. The lens must hold instead.
    Tracker t(1920, 1080, 1.0);
    t.update(960, 540, 0, 0);
    t.update(960, 540, 50, 0);            // lone alias tick
    CHECK(t.centerX() == doctest::Approx(960));   // held, NOT 1010
    CHECK_FALSE(t.locked());
}

TEST_CASE("interleaved moved/frozen ticks track the cursor without oscillating") {
    Tracker t(1920, 1080, 1.0);
    t.update(500, 500, 0, 0);
    t.update(504, 500, 40, 0);            // moved -> snap, resets lock counter
    CHECK(t.centerX() == doctest::Approx(504));
    t.update(504, 500, 40, 0);            // alias tick -> hold (not 544)
    CHECK(t.centerX() == doctest::Approx(504));
    t.update(508, 500, 40, 0);            // moved -> snap to true cursor
    CHECK(t.centerX() == doctest::Approx(508));
    CHECK_FALSE(t.locked());              // never falsely locked
}

// --- Locked mode (games that hide/clip/lock the cursor) ------------------------

TEST_CASE("lock engages only after sustained freeze, then integrates raw deltas") {
    Tracker t(1920, 1080, 1.0);
    t.update(960, 540, 0, 0);                       // establish
    for (int i = 0; i < Tracker::kLockEngageTicks - 1; ++i)
        t.update(960, 540, 5, 0);                   // ramp: held, not integrated
    CHECK(t.centerX() == doctest::Approx(960));
    CHECK_FALSE(t.locked());
    t.update(960, 540, 5, 0);                       // threshold tick: locks + integrates
    CHECK(t.locked());
    CHECK(t.centerX() == doctest::Approx(965));
    t.update(960, 540, 10, 0);                      // stays locked, keeps integrating
    CHECK(t.centerX() == doctest::Approx(975));
}

TEST_CASE("sensitivity scales locked-mode panning") {
    Tracker t(1920, 1080, 2.0);
    t.update(960, 540, 0, 0);
    for (int i = 0; i < Tracker::kLockEngageTicks - 1; ++i)
        t.update(960, 540, 1, 0);                   // ramp (held)
    CHECK(t.centerX() == doctest::Approx(960));
    t.update(960, 540, 10, 0);                      // engages + integrates: 10 * 2.0
    CHECK(t.centerX() == doctest::Approx(980));
}

TEST_CASE("returns to free mode and snaps to cursor when it moves again") {
    Tracker t(1920, 1080, 1.0);
    t.update(960, 540, 0, 0);
    for (int i = 0; i < Tracker::kLockEngageTicks; ++i)
        t.update(960, 540, 50, 0);                  // engage + pan in locked mode
    CHECK(t.locked());
    t.update(400, 400, 0, 0);                       // OS cursor moved -> free, snap
    CHECK(t.centerX() == doctest::Approx(400));
    CHECK(t.centerY() == doctest::Approx(400));
    CHECK_FALSE(t.locked());
}

TEST_CASE("clamps center to screen bounds in locked mode") {
    Tracker t(1920, 1080, 1.0);
    t.update(10, 10, 0, 0);
    for (int i = 0; i < Tracker::kLockEngageTicks - 1; ++i)
        t.update(10, 10, -1, -1);                   // ramp (held)
    t.update(10, 10, -1000, -1000);                 // engages + integrates, clamps
    CHECK(t.centerX() == doctest::Approx(0));
    CHECK(t.centerY() == doctest::Approx(0));
}

TEST_CASE("recenter snaps to screen center") {
    Tracker t(1920, 1080, 1.0);
    t.update(100, 100, 0, 0);
    t.recenter();
    CHECK(t.centerX() == doctest::Approx(960));
    CHECK(t.centerY() == doctest::Approx(540));
}
```

- [ ] **Step 2: Run tests, verify the flicker/lock tests FAIL on current code**

Run: `build.bat test`
Expected: the new "lone frozen-cursor", "interleaved", and "lock engages only after
sustained freeze" cases FAIL (current code locks on a single tick, so the lens jumps).

- [ ] **Step 3: Add lock state + threshold to `src/tracker.h`**

Replace the class body so it reads:

```cpp
#pragma once
namespace wind {
// Pure tracking state. Caller samples GetCursorPos and the summed raw-input deltas
// since the last tick, then calls update() once per tick.
//
// Two modes chosen by a hysteresis lock detector:
//   Free   - OS cursor moving normally -> lens follows GetCursorPos.
//   Locked - OS cursor frozen (a game hid/clipped/locked it) while the hand keeps
//            moving (raw deltas arrive) -> lens pans by raw deltas. Wind's core feature.
//
// A *single* tick that sees an unchanged GetCursorPos while raw deltas arrive is NOT a
// lock: during free movement that is a sampling alias, and integrating it makes the lens
// jump off-centre then snap back (visible flicker). The lock engages only after
// kLockEngageTicks consecutive frozen-cursor-with-raw ticks and disengages immediately
// when the OS cursor moves again.
class Tracker {
public:
    // Consecutive frozen-cursor + raw ticks needed to treat the cursor as locked. At a
    // 60-144 Hz tick this is ~40-100 ms: far longer than any free-movement sampling
    // alias, and a one-time imperceptible delay when a real game lock engages.
    static constexpr int kLockEngageTicks = 6;

    Tracker(int screenW, int screenH, double sensitivity);
    void update(int cursorX, int cursorY, int rawDx, int rawDy);
    void recenter();
    double centerX() const { return cx_; }
    double centerY() const { return cy_; }
    bool   locked()  const { return locked_; }
private:
    void clamp();
    int screenW_, screenH_;
    double sensitivity_;
    double cx_, cy_;
    int lastCursorX_, lastCursorY_;
    bool haveCursor_ = false;
    bool locked_ = false;
    int  stationaryRawTicks_ = 0;
};
}
```

- [ ] **Step 4: Rewrite `Tracker::update` in `src/tracker.cpp`**

Replace the `update` function body with:

```cpp
void Tracker::update(int cursorX, int cursorY, int rawDx, int rawDy) {
    bool cursorMoved = !haveCursor_ || cursorX != lastCursorX_ || cursorY != lastCursorY_;
    bool rawMoved = (rawDx != 0 || rawDy != 0);

    if (cursorMoved) {
        // OS cursor moving freely: follow it and abandon any lock at once.
        locked_ = false;
        stationaryRawTicks_ = 0;
        cx_ = cursorX;
        cy_ = cursorY;
    } else if (rawMoved) {
        // OS cursor frozen but the hand is moving: candidate for a cursor lock.
        if (!locked_ && ++stationaryRawTicks_ >= kLockEngageTicks)
            locked_ = true;
        if (locked_) {
            cx_ += rawDx * sensitivity_;   // pan the lens with raw movement
            cy_ += rawDy * sensitivity_;
        }
        // While not yet locked: hold the centre (do NOT jump). Removes the flicker.
    } else {
        // Neither cursor nor hand moved: hold. Reset the engage counter so only
        // *consecutive* frozen+raw ticks arm a lock; an established lock_ stays on
        // (a locked game cursor remains frozen when you briefly stop moving).
        stationaryRawTicks_ = 0;
    }
    clamp();
    lastCursorX_ = cursorX;
    lastCursorY_ = cursorY;
    haveCursor_ = true;
}
```

- [ ] **Step 5: Run tests, verify all pass**

Run: `build.bat test`
Expected: SUCCESS, all cases pass (25 cases total).

- [ ] **Step 6: Commit**

```bash
git add src/tracker.h src/tracker.cpp tests/test_tracker.cpp
git commit -m "fix: tracker lock detector uses hysteresis, killing desktop flicker"
```

---

## Task 2: Raw-Input side-button detection (Issue 1, best-effort)

**Files:**
- Modify: `src/main.cpp`

Not unit-testable (Win32 input over elevated windows requires live testing). Implement
the reasoned fix; it compiles and is additive/idempotent with the existing hook.

- [ ] **Step 1: Add zoom-button id statics + a setter above `WndProc` in `src/main.cpp`**

After `static InputRouter g_input;` add:

```cpp
static int g_zoomInBtnId = 2;   // XBUTTON id: 1 = XBUTTON1, 2 = XBUTTON2 (set from cfg)
static int g_zoomOutBtnId = 1;

// Set side-button state from a Raw Input transition. Mirrors the hook's mapping so the
// two state sources are interchangeable and idempotent.
static void SetZoomButton(int xbuttonId, bool down) {
    if (xbuttonId == g_zoomInBtnId)  g_input.state().inHeld.store(down);
    if (xbuttonId == g_zoomOutBtnId) g_input.state().outHeld.store(down);
}
```

- [ ] **Step 2: Decode XBUTTON flags in the `WM_INPUT` handler**

Replace the mouse-handling block in `WndProc` (the `if (ri->header.dwType == RIM_TYPEMOUSE ...)` section) with:

```cpp
            if (ri->header.dwType == RIM_TYPEMOUSE) {
                const RAWMOUSE& m = ri->data.mouse;
                if ((m.usFlags & MOUSE_MOVE_ABSOLUTE) == 0) {
                    AccumulateRaw(g_input, m.lLastX, m.lLastY);
                }
                // Side-button state via Raw Input. This path is delivered even when an
                // elevated window (Task Manager, UAC) is foreground, where the
                // WH_MOUSE_LL hook is not invoked for a medium-IL process (UIPI). The
                // hook still runs for normal windows (and swallows the buttons there);
                // setting the same state from both sources is idempotent.
                USHORT bf = m.usButtonFlags;
                if (bf & RI_MOUSE_BUTTON_4_DOWN) SetZoomButton(1, true);
                if (bf & RI_MOUSE_BUTTON_4_UP)   SetZoomButton(1, false);
                if (bf & RI_MOUSE_BUTTON_5_DOWN) SetZoomButton(2, true);
                if (bf & RI_MOUSE_BUTTON_5_UP)   SetZoomButton(2, false);
            }
```

- [ ] **Step 3: Set the button ids from config in `wWinMain`**

Immediately after `Config cfg = LoadConfig(L"magnifier.ini");` add:

```cpp
    g_zoomInBtnId = cfg.zoomInButton;
    g_zoomOutBtnId = cfg.zoomOutButton;
```

- [ ] **Step 4: Build the app, verify it compiles**

Run: `build.bat`
Expected: compiles, `Wind.exe` produced, no errors.

- [ ] **Step 5: Commit**

```bash
git add src/main.cpp
git commit -m "fix: read zoom side-buttons from Raw Input so they work over elevated windows"
```

---

## Task 3: Diagnostics lock counter + docs (verification aid)

**Files:**
- Modify: `src/main.cpp`
- Modify: `docs/KNOWN-ISSUES.md`

- [ ] **Step 1: Count locked ticks per diagnostics window in `src/main.cpp`**

Add a `winLockedTicks` counter alongside the other window accumulators, increment it
when `tracker.locked()` each tick (inside the `if (diag)` block), emit it on the diag
line as `lockedTicks=<n>/<iters>`, and reset it with the others. This lets a desktop
session confirm locked ticks stay ~0 (no false locks) and a game session confirm they
climb (lock engages).

- [ ] **Step 2: Build app, verify compiles**

Run: `build.bat`
Expected: compiles cleanly.

- [ ] **Step 3: Update `docs/KNOWN-ISSUES.md` statuses**

Mark Issues 2 and 3 as fixed-and-unit-tested (root cause confirmed: the tracker
single-tick heuristic), Issue 1 as implemented-pending-live-verification, with the exact
user test steps.

- [ ] **Step 4: Commit**

```bash
git add src/main.cpp docs/KNOWN-ISSUES.md docs/superpowers/plans/2026-05-25-interaction-fixes.md
git commit -m "diag: per-window locked-tick counter; update KNOWN-ISSUES status"
```

---

## Self-Review

- **Spec coverage:** Issue 3 (flicker) -> Task 1 hysteresis. Issue 2 (mis-click / no
  I-beam) -> resolved by Task 1 (center stays on the true cursor, so visual-only mapping
  is correct again); confirmed by reasoning, needs user click-test. Issue 1 (elevated
  windows) -> Task 2 Raw Input. Issue 4 (game FPS) -> out of scope (API ceiling, separate
  decision). Covered.
- **No placeholders:** all code is concrete except Task 3 Step 1, described precisely
  (counter + emit + reset) - implement inline.
- **Type consistency:** `kLockEngageTicks`, `locked()`, `SetZoomButton`,
  `g_zoomInBtnId/g_zoomOutBtnId` used consistently across tasks.
- **Core feature preserved:** locked-mode raw integration still happens; only its
  engage condition changed (hysteresis), verified by the locked-mode tests.

## Verification & limits

- Tasks 1 fully proven by deterministic unit tests (no live testing needed).
- Task 2 cannot be auto-verified (needs the user to test zoom over Task Manager). The
  reasoning: Raw Input INPUTSINK delivery is not gated by the UIPI rule that suppresses a
  medium-IL low-level hook over elevated windows. Risk: theoretical chance the LL-hook
  swallow suppresses the cooked message but not the raw report (expected: it does not).
- Issue 2's real-world resolution depends on the same root cause as Issue 3; if the user
  still sees mis-clicks after the tracker fix, re-open with new evidence.
