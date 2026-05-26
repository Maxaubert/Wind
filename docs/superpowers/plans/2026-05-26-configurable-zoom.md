# Configurable Zoom Experience Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add per-direction zoom-speed multipliers and an opt-in "smooth zoom" mode (zoom-in accelerates while held), all configurable, with defaults that reproduce today's linear zoom exactly.

**Architecture:** All zoom logic stays in the pure, unit-tested `ZoomController`. A new `setProfile(...)` carries 5 config-driven parameters (in/out speed multipliers, smooth toggle, accel amount, ramp seconds) plus a `heldIn_` timer for the accel ramp. `RunTick` calls `setProfile` each frame from the live config (free hot-reload, no level reset). No render-engine changes.

**Tech Stack:** C++17, MSVC. Pure logic in `src/zoom_controller.*` and `src/config.*` (no `<windows.h>`), compiled into the doctest unit-test binary. Build: `build.bat` (app), `build.bat test` (tests, exit 0 = pass).

**Spec:** `docs/superpowers/specs/2026-05-26-configurable-zoom-design.md`

**Branch:** `feat/zoom-config` (already created off `main`).

---

## File structure

- `src/config.h` - add 5 `Config` fields (the new knobs, with defaults).
- `src/config.cpp` - parse the 5 keys in `ParseConfig`; write them in `LoadConfig`'s default-ini text.
- `tools/uiaccess_setup.ps1` - add the 5 keys to the deployed default ini.
- `tests/test_config.cpp` - assert defaults + parsing of the 5 keys.
- `src/zoom_controller.h` - add `setProfile(...)`, the profile members, and `heldIn_`.
- `src/zoom_controller.cpp` - implement `setProfile`, the speed/accel math in `tick`, reset `heldIn_`.
- `tests/test_zoom_controller.cpp` - new tests for speeds, smooth ramp, reset, out-ignores-accel, guards. Existing tests are the regression guard.
- `src/main.cpp` - in `RunTick`, call `t.zoom.setProfile(...)` from `t.cfg` each frame.

---

### Task 1: Config knobs

**Files:**
- Modify: `src/config.h` (after `double fullRangeSeconds = 1.2;`)
- Modify: `src/config.cpp` (`ParseConfig` else-if chain; `LoadConfig` default text)
- Modify: `tools/uiaccess_setup.ps1` (default ini here-string)
- Test: `tests/test_config.cpp`

- [ ] **Step 1: Write the failing tests**

In `tests/test_config.cpp`, add to the existing `TEST_CASE("renderer knobs have sane defaults")` block (right after the `CHECK(c.multiMonitor == 1);` line):

```cpp
    CHECK(c.smoothZoom == 0);                  // linear (current) by default
    CHECK(c.zoomInSpeed == doctest::Approx(1.0));
    CHECK(c.zoomOutSpeed == doctest::Approx(1.0));
    CHECK(c.smoothZoomAccel == doctest::Approx(3.0));
    CHECK(c.smoothZoomRamp == doctest::Approx(0.6));
```

And add a new standalone test case at the end of the file:

```cpp
TEST_CASE("zoom-speed and smooth-zoom knobs parse") {
    Config c = ParseConfig(
        "smoothZoom=1\nzoomInSpeed=2.0\nzoomOutSpeed=0.5\n"
        "smoothZoomAccel=4.0\nsmoothZoomRamp=0.25\n");
    CHECK(c.smoothZoom == 1);
    CHECK(c.zoomInSpeed == doctest::Approx(2.0));
    CHECK(c.zoomOutSpeed == doctest::Approx(0.5));
    CHECK(c.smoothZoomAccel == doctest::Approx(4.0));
    CHECK(c.smoothZoomRamp == doctest::Approx(0.25));
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `build.bat test`
Expected: compile error - `Config` has no member `smoothZoom` (etc.). That is the failing state.

- [ ] **Step 3: Add the fields to `Config`**

In `src/config.h`, immediately after the line `double fullRangeSeconds = 1.2;`, insert:

```cpp
    // --- Zoom experience (see docs/superpowers/specs/2026-05-26-configurable-zoom-design.md) ---
    // Per-direction rate multipliers (1.0 = today's speed); apply in BOTH linear and smooth modes.
    double zoomInSpeed  = 1.0;       // 0.25-4.0
    double zoomOutSpeed = 1.0;       // 0.25-4.0
    // Smooth zoom: 0 = linear/constant (default); 1 = zoom-IN accelerates while held.
    int    smoothZoom = 0;
    // Smooth-mode top zoom-in rate = zoomInSpeed * smoothZoomAccel (>=1; 1 = no accel). 1.0-8.0.
    double smoothZoomAccel = 3.0;
    // Seconds of continuous holding to reach the top zoom-in rate. 0.1-3.0.
    double smoothZoomRamp = 0.6;
```

- [ ] **Step 4: Parse the keys in `ParseConfig`**

In `src/config.cpp`, in the `else if` chain inside `ParseConfig`, after the `fullRangeSeconds` line
(`else if (key == "fullRangeSeconds") c.fullRangeSeconds = std::stod(val);`), add:

```cpp
            else if (key == "zoomInSpeed")      c.zoomInSpeed = std::stod(val);
            else if (key == "zoomOutSpeed")     c.zoomOutSpeed = std::stod(val);
            else if (key == "smoothZoom")       c.smoothZoom = std::stoi(val);
            else if (key == "smoothZoomAccel")  c.smoothZoomAccel = std::stod(val);
            else if (key == "smoothZoomRamp")   c.smoothZoomRamp = std::stod(val);
```

- [ ] **Step 5: Write the keys in `LoadConfig`'s default ini**

In `src/config.cpp`, in `LoadConfig`, find the default-text line `"maxLevel=8.0\nfullRangeSeconds=1.2\n"`
and replace it with:

```cpp
               "maxLevel=8.0\nfullRangeSeconds=1.2\n"
               "; zoomInSpeed/zoomOutSpeed: zoom rate multipliers (1.0=default, 2.0=twice as fast, 0.5=half)\n"
               "zoomInSpeed=1.0\nzoomOutSpeed=1.0\n"
               "; smoothZoom: 0=linear constant speed (default); 1=zoom-IN accelerates the longer you hold\n"
               "smoothZoom=0\n"
               "; smoothZoomAccel: smooth-mode top zoom-in rate = zoomInSpeed * this (1=no accel)\n"
               "smoothZoomAccel=3.0\n"
               "; smoothZoomRamp: seconds of holding to reach the top zoom-in rate\n"
               "smoothZoomRamp=0.6\n"
```

- [ ] **Step 6: Add the keys to the deployed default ini**

In `tools/uiaccess_setup.ps1`, in the `$ini = @"..."@` here-string, after the
`fullRangeSeconds=1.2` line, add:

```
zoomInSpeed=1.0
zoomOutSpeed=1.0
smoothZoom=0
smoothZoomAccel=3.0
smoothZoomRamp=0.6
```

- [ ] **Step 7: Run tests to verify they pass**

Run: `build.bat test`
Expected: PASS - all cases pass, including the two new assertions blocks.

- [ ] **Step 8: Commit**

```bash
git add src/config.h src/config.cpp tools/uiaccess_setup.ps1 tests/test_config.cpp
git commit -m "feat: add zoom-speed + smooth-zoom config knobs (defaults = current behavior)"
```

---

### Task 2: ZoomController speed + smooth-accel model

**Files:**
- Modify: `src/zoom_controller.h`
- Modify: `src/zoom_controller.cpp`
- Test: `tests/test_zoom_controller.cpp`

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_zoom_controller.cpp` (note: `#include <cmath>` at the top of the file first, for `std::pow`):

```cpp
TEST_CASE("zoomInSpeed multiplies the in rate") {
    ZoomController z(1.0, 8.0, 1.2);
    z.setProfile(2.0, 1.0, false, 3.0, 0.6);   // 2x in-speed
    z.setDirection(ZoomDir::In);
    z.tick(0.6);                                // 2x for 0.6s == 1x for 1.2s == full range
    CHECK(z.level() == doctest::Approx(8.0));
}
TEST_CASE("zoomOutSpeed multiplies the out rate") {
    ZoomController z(1.0, 8.0, 1.2);
    z.setDirection(ZoomDir::In); z.tick(1.2);  // at 8.0 (default profile)
    z.setProfile(1.0, 2.0, false, 3.0, 0.6);   // 2x out-speed
    z.setDirection(ZoomDir::Out); z.tick(0.6); // 2x for 0.6s == full range back down
    CHECK(z.level() == doctest::Approx(1.0));
}
TEST_CASE("smooth zoom-in outpaces linear over the same hold") {
    ZoomController lin(1.0, 1e9, 1.2);  // huge max so neither clamps
    ZoomController sm (1.0, 1e9, 1.2);
    lin.setProfile(1.0, 1.0, false, 3.0, 0.2);
    sm .setProfile(1.0, 1.0, true,  3.0, 0.2);
    lin.setDirection(ZoomDir::In); sm.setDirection(ZoomDir::In);
    for (int i = 0; i < 20; ++i) { lin.tick(0.05); sm.tick(0.05); }  // 1.0s, well past the 0.2s ramp
    CHECK(sm.level() > lin.level());
}
TEST_CASE("smooth zoom plateaus at inSpeed*accel rate") {
    ZoomController z(1.0, 8.0, 100.0);          // slow base so it won't clamp
    z.setProfile(1.0, 1.0, true, 2.0, 0.1);     // accel 2x, ramp 0.1s
    z.setDirection(ZoomDir::In);
    for (int i = 0; i < 5; ++i) z.tick(0.05);   // heldIn 0.25s, well past ramp -> plateau
    double l1 = z.level();
    z.tick(0.1);
    double l2 = z.level();
    // At plateau the per-tick factor is pow(R, dt*inSpeed*accel/T) with R=8, inSpeed=1, accel=2.
    CHECK(l2 == doctest::Approx(l1 * std::pow(8.0, 0.1 * 2.0 / 100.0)));
}
TEST_CASE("releasing resets the smooth ramp so the next in starts slow") {
    ZoomController z(1.0, 8.0, 100.0);
    z.setProfile(1.0, 1.0, true, 4.0, 0.1);
    z.setDirection(ZoomDir::In);
    for (int i = 0; i < 5; ++i) z.tick(0.05);   // warmed to plateau (fast)
    double warmBefore = z.level();
    z.tick(0.02); double warmGain = z.level() - warmBefore;   // a fast (plateau) increment
    z.setDirection(ZoomDir::None); z.tick(0.1); // release -> heldIn resets to 0
    z.setDirection(ZoomDir::In);
    double freshBefore = z.level();
    z.tick(0.02); double freshGain = z.level() - freshBefore;  // should be a slow (near-base) increment
    CHECK(freshGain < warmGain);
}
TEST_CASE("zoom-out ignores smooth acceleration") {
    ZoomController z(1.0, 8.0, 1.2);
    z.setDirection(ZoomDir::In); z.tick(1.2);   // at 8.0
    z.setProfile(1.0, 1.0, true, 5.0, 0.1);     // smooth on, big accel
    z.setDirection(ZoomDir::Out); z.tick(0.6);  // out should be plain 1x rate (no accel)
    CHECK(z.level() == doctest::Approx(8.0 / std::pow(8.0, 0.5)));  // == 2.8284, the linear out result
}
TEST_CASE("smooth-zoom guards: accel<1 and ramp=0 don't break the curve") {
    ZoomController a(1.0, 8.0, 1.2);
    a.setProfile(1.0, 1.0, true, 0.5, 0.0);     // accel<1 and ramp=0
    a.setDirection(ZoomDir::In); a.tick(0.6);
    CHECK(a.level() == doctest::Approx(2.8284).epsilon(0.001));  // accel<1 ignored -> linear midpoint
    ZoomController b(1.0, 1e9, 1.2);
    b.setProfile(1.0, 1.0, true, 2.0, 0.0);     // ramp=0 -> instant top, must not divide by zero
    b.setDirection(ZoomDir::In); b.tick(0.01);
    CHECK(b.level() > 1.0);                      // finite, advanced (no NaN/inf)
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `build.bat test`
Expected: compile error - `ZoomController` has no member `setProfile`. That is the failing state.

- [ ] **Step 3: Add `setProfile` + members to the header**

Replace the body of `class ZoomController` in `src/zoom_controller.h` with:

```cpp
class ZoomController {
public:
    ZoomController(double minLevel, double maxLevel, double fullRangeSeconds);
    void setDirection(ZoomDir d);
    ZoomDir direction() const { return dir_; }
    // Speed/acceleration profile (hot-reloadable; does NOT reset the level):
    //   inSpeed/outSpeed - per-direction rate multipliers (1.0 = base; both modes)
    //   smooth           - accelerate zoom-IN while held
    //   accel            - smooth-mode top in-rate = inSpeed * accel (clamped >=1; 1 = no accel)
    //   rampSeconds      - seconds of continuous zoom-in to reach the top in-rate (<=0 = instant)
    void setProfile(double inSpeed, double outSpeed, bool smooth, double accel, double rampSeconds);
    void tick(double dtSeconds);   // ramp level multiplicatively toward bound
    double level() const { return level_; }
    void reset();                  // level=min, dir=None, held cleared
private:
    double minLevel_, maxLevel_, fullRangeSeconds_;
    double level_;
    ZoomDir dir_ = ZoomDir::None;
    double inSpeed_ = 1.0, outSpeed_ = 1.0;   // defaults reproduce today's behavior
    bool   smooth_ = false;
    double accel_ = 3.0, rampSeconds_ = 0.6;
    double heldIn_ = 0.0;                      // continuous seconds zoom-in held (drives accel ramp)
};
```

- [ ] **Step 4: Implement `setProfile` + the math in `tick` + reset**

Replace `src/zoom_controller.cpp` with:

```cpp
#include "zoom_controller.h"
#include <algorithm>
#include <cmath>
namespace wind {
ZoomDir ResolveDirection(bool inHeld, bool outHeld) {
    if (inHeld == outHeld) return ZoomDir::None; // neither, or both
    return inHeld ? ZoomDir::In : ZoomDir::Out;
}
ZoomController::ZoomController(double minLevel, double maxLevel, double fullRangeSeconds)
    : minLevel_(minLevel), maxLevel_(maxLevel),
      fullRangeSeconds_(fullRangeSeconds), level_(minLevel) {}
void ZoomController::setDirection(ZoomDir d) { dir_ = d; }
void ZoomController::setProfile(double inSpeed, double outSpeed, bool smooth,
                                double accel, double rampSeconds) {
    inSpeed_ = inSpeed; outSpeed_ = outSpeed; smooth_ = smooth;
    accel_ = accel; rampSeconds_ = rampSeconds;
}
void ZoomController::tick(double dt) {
    // Track continuous zoom-in hold time for the smooth-zoom accel ramp; any non-In direction
    // (release or reverse) resets it, so each fresh zoom-in starts slow again.
    if (dir_ == ZoomDir::In && dt > 0.0) heldIn_ += dt;
    else if (dir_ != ZoomDir::In)        heldIn_ = 0.0;
    if (dir_ == ZoomDir::None || dt <= 0.0) return;

    double speed;
    if (dir_ == ZoomDir::In) {
        double accelMult = 1.0;
        if (smooth_ && accel_ > 1.0) {                 // accel<=1 -> no acceleration
            double t = (rampSeconds_ > 0.0 && heldIn_ < rampSeconds_)
                         ? heldIn_ / rampSeconds_       // 0..1 ramp (ramp<=0 -> instant top)
                         : 1.0;
            accelMult = 1.0 + (accel_ - 1.0) * t;       // 1..accel
        }
        speed = inSpeed_ * accelMult;
    } else {
        speed = outSpeed_;                              // out never accelerates
    }
    double f = std::pow(maxLevel_ / minLevel_, dt * speed / fullRangeSeconds_);
    if (dir_ == ZoomDir::In)  level_ *= f;
    else                      level_ /= f;
    level_ = std::min(maxLevel_, std::max(minLevel_, level_));
}
void ZoomController::reset() { level_ = minLevel_; dir_ = ZoomDir::None; heldIn_ = 0.0; }
}
```

- [ ] **Step 5: Run tests to verify they pass**

Run: `build.bat test`
Expected: PASS - the 7 new cases AND all pre-existing zoom tests (the regression guard: with default
profile, `tick` is byte-for-byte the old `pow` step).

- [ ] **Step 6: Commit**

```bash
git add src/zoom_controller.h src/zoom_controller.cpp tests/test_zoom_controller.cpp
git commit -m "feat: ZoomController speed multipliers + smooth zoom-in acceleration"
```

---

### Task 3: Wire the live config into RunTick

**Files:**
- Modify: `src/main.cpp` (`RunTick`, at the zoom drive point)

- [ ] **Step 1: Add the `setProfile` call**

In `src/main.cpp`, in `RunTick`, find:

```cpp
    t.zoom.setDirection(ResolveDirection(inHeld, outHeld));
```

and insert immediately ABOVE it:

```cpp
    // Apply the live zoom profile every frame (free hot-reload; setProfile does not reset level).
    t.zoom.setProfile(t.cfg.zoomInSpeed, t.cfg.zoomOutSpeed, t.cfg.smoothZoom != 0,
                      t.cfg.smoothZoomAccel, t.cfg.smoothZoomRamp);
```

(The existing dt-clamp on `t.zoom.tick(...)` just below stays unchanged.)

- [ ] **Step 2: Build the app**

Run: `build.bat`
Expected: exit 0, `Wind.exe` produced. (Kill any running Wind first if the link fails with LNK1104.)

- [ ] **Step 3: Smoke-test the render path is intact**

Run (PowerShell): `Start-Process .\Wind.exe -Environment @{ WIND_SELFTEST = "1" } -Wait`
Expected: `wind_selftest.png` is written and shows a normal magnified frame (zoom config does not
touch the render path, so this only confirms nothing regressed in the build/integration).

- [ ] **Step 4: Commit**

```bash
git add src/main.cpp
git commit -m "feat: feed live zoom profile (speeds + smooth zoom) into the tick"
```

---

## Manual verification (after all tasks)

Defaults must feel identical to today. Then, editing `magnifier.ini` (hot-reloads in ~1s):
- `zoomInSpeed=2.0` -> zoom-in reaches max in ~0.6s; `0.5` -> ~2.4s. `zoomOutSpeed` likewise for out.
- `smoothZoom=1` -> zoom-in starts at `zoomInSpeed` and accelerates to `zoomInSpeed*smoothZoomAccel`
  over `smoothZoomRamp` seconds, then holds steady; releasing and re-holding starts slow again;
  zoom-out stays constant.

## Notes for the implementer

- Pure-logic files (`config.cpp` parse half, `zoom_controller.cpp`) MUST NOT include `<windows.h>`;
  the test build compiles only the pure `.cpp` files. `<algorithm>`/`<cmath>` are fine.
- The default profile (`smoothZoom=0`, speeds `1.0`) makes `tick` mathematically identical to the
  current implementation - the pre-existing `test_zoom_controller.cpp` cases are the regression guard
  and must keep passing untouched.
- Do NOT change `maxLevel` / `fullRangeSeconds` semantics, and do NOT touch `render_engine`.
