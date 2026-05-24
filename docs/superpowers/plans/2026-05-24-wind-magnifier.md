# Wind Magnifier Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build "Wind", a lightweight standalone Windows fullscreen magnifier that replaces `Magnify.exe`, with smooth continuous zoom and a lens that keeps tracking the mouse even when games hide/clip/center-lock the cursor.

**Architecture:** A small C++ Win32 app. Pure logic (zoom ramp, cursor tracking, offset math, config parsing) is isolated into header/cpp pairs with no Win32 dependency and is unit-tested with doctest. A thin I/O shell wraps the Windows Magnification API (zoom), Raw Input + a low-level mouse hook (input that survives cursor lock), an INI config, and a tray icon. A tick thread paced by `DwmFlush` reads the shared state each compositor frame and calls `MagSetFullscreenTransform`.

**Tech Stack:** C++17, MSVC `cl.exe` located via `vswhere`, Windows Magnification API (`Magnification.lib`), Raw Input, `WH_MOUSE_LL`, DWM (`Dwmapi.lib`), doctest (single-header tests).

---

## PR / issue grouping

Per the repo's GitHub workflow, land the plan as four issues → branches → PRs:

- **PR1 (Task 1-3):** Repo scaffold + live verification loop (build + passing test).
- **PR2 (Task 4-7):** Pure logic modules, TDD.
- **PR3 (Task 8-10):** I/O modules (Magnification engine, input, config file).
- **PR4 (Task 11-13):** Wiring, tray, manual verification + tuning.

Open one GitHub issue per PR, branch from `main` (in a worktree), and reference the issue in the PR.

## File structure

```
Wind/
├── CLAUDE.md                  # build/run/test commands, stack, gotchas
├── README.md                  # what it is, usage, config
├── build.bat                  # locate MSVC via vswhere; build Wind.exe and tests
├── .gitignore
├── Wind.manifest              # Per-Monitor-V2 DPI awareness
├── config/magnifier.ini       # default config shipped next to exe
├── .claude/settings.json      # permission allowlist + build/test hooks
├── third_party/doctest.h      # single-header test framework (vendored)
├── src/
│   ├── transform.h / .cpp         # PURE: (center,level,screen) -> offset
│   ├── zoom_controller.h / .cpp   # PURE: hold-to-zoom ramp + ResolveDirection
│   ├── tracker.h / .cpp           # PURE: free/locked blend, delta integration, clamp
│   ├── config.h / .cpp            # PURE parse + I/O load/save + mtime hot-reload
│   ├── magnifier_engine.h / .cpp  # I/O: Magnification API wrapper
│   ├── input_router.h / .cpp      # I/O: Raw Input + WH_MOUSE_LL, shared atomics
│   ├── tray.h / .cpp              # I/O: tray icon + menu
│   └── main.cpp                   # WinMain, single-instance, msg loop, tick thread
└── tests/
    ├── test_main.cpp              # #define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
    ├── test_transform.cpp
    ├── test_zoom_controller.cpp
    ├── test_tracker.cpp
    └── test_config.cpp
```

**Design boundary:** `transform`, `zoom_controller`, `tracker`, and the parse half of `config` include only `<algorithm>`/`<cmath>`/`<string>` — never `<windows.h>` — so tests compile and run without a desktop session. All Win32 calls live in `magnifier_engine`, `input_router`, `tray`, the I/O half of `config`, and `main`.

---

## Task 1: Repo skeleton + GitHub issue/branch (PR1)

**Files:**
- Create: `.gitignore`, `README.md`, `CLAUDE.md`

- [ ] **Step 1: Open the issue and branch**

```bash
gh issue create --title "PR1: repo scaffold + verification loop" \
  --body "Scaffold Wind: .gitignore, README, CLAUDE.md, .claude settings, build.bat, doctest, first passing test."
git checkout -b feat/scaffold
```
(If `gh repo create` has not been run yet, create the remote first: `gh repo create Wind --private --source . --remote origin`. This publishes a private repo — confirm with the user before running.)

- [ ] **Step 2: Write `.gitignore`**

```gitignore
# Build output
*.obj
*.exe
*.pdb
*.ilk
*.lib
*.exp
build/
# Editor / OS
.vs/
*.user
Thumbs.db
# Runtime config copy (the shipped default lives in config/)
/magnifier.ini
```

- [ ] **Step 3: Write `README.md`**

```markdown
# Wind

A lightweight fullscreen magnifier for Windows — "light as air". A replacement for
the built-in Magnifier, with smooth continuous zoom that keeps tracking the mouse
even when games hide, clip, or center-lock the cursor.

## Controls (default, configurable in `magnifier.ini`)
- Hold **mouse forward button (XButton2)** — zoom in (smooth ramp).
- Hold **mouse back button (XButton1)** — zoom out (smooth ramp).
- Release — zoom stays at the current level.

## Build
Requires Visual Studio 2022+ Build Tools. Run `build.bat` from any shell.
- `build.bat` — builds `Wind.exe`.
- `build.bat test` — builds and runs unit tests.

## Run
`Wind.exe` — runs from the system tray. Right-click the tray icon to edit config or quit.

## Scope
v1 covers the desktop, normal apps, and **borderless / windowed-fullscreen** games.
Exclusive-fullscreen games are out of scope for v1 (set the game to borderless).
```

- [ ] **Step 4: Write `CLAUDE.md`**

```markdown
# Wind — fullscreen magnifier

Lightweight standalone Windows fullscreen magnifier replacing Magnify.exe.
Design spec: `docs/superpowers/specs/2026-05-24-magnifier-design.md`.

## Commands
- Build app: `build.bat`  (locates MSVC via vswhere, emits `Wind.exe`)
- Build + run tests: `build.bat test`  (runs the doctest binary; exit 0 = pass)

## Stack
C++17, MSVC cl.exe. Windows Magnification API (`Magnification.lib`), Raw Input,
`WH_MOUSE_LL`, DWM (`Dwmapi.lib`). Tests: vendored `third_party/doctest.h`.

## Architecture
Pure logic (no `<windows.h>`): `src/transform`, `src/zoom_controller`, `src/tracker`,
parse half of `src/config`. Win32 I/O: `magnifier_engine`, `input_router`, `tray`,
`main`. A `DwmFlush`-paced tick thread reads shared atomics and calls
`MagSetFullscreenTransform(level, xOffset, yOffset)` each frame.

## IMPORTANT gotchas
- Pure-logic files MUST NOT include `<windows.h>` — keeps unit tests desktop-free.
- Declare Per-Monitor-V2 DPI awareness (`Wind.manifest`) or offset pixel math is wrong
  on scaled displays.
- Always reset to `MagSetFullscreenTransform(1.0,0,0)` + `MagUninitialize` on exit —
  never leave the screen zoomed.
- The lens-must-move-when-cursor-locked behavior is THE core feature. It relies on
  Raw Input deltas (HID-level, unaffected by ShowCursor/ClipCursor/SetCursorPos),
  NOT GetCursorPos, when a lock is detected. Do not "simplify" this away.
- `MagSetInputTransform` is intentionally NOT used (needs UIAccess). Visual-only.

## Workflow
Feature/fix work: GitHub issue → branch → PR. README-only changes commit directly.
```

- [ ] **Step 5: Commit**

```bash
git add .gitignore README.md CLAUDE.md
git commit -m "chore: repo skeleton (gitignore, README, CLAUDE.md)"
```

---

## Task 2: `.claude/settings.json` (permissions + hooks) (PR1)

**Files:**
- Create: `.claude/settings.json`

- [ ] **Step 1: Write settings with a build/test permission allowlist and a post-edit build-test hook**

```json
{
  "permissions": {
    "allow": [
      "Bash(build.bat)",
      "Bash(build.bat test)",
      "Bash(git add *)",
      "Bash(git commit *)",
      "Bash(git checkout *)",
      "Bash(git status)",
      "Bash(git diff *)",
      "Bash(gh issue *)",
      "Bash(gh pr *)"
    ],
    "ask": [],
    "deny": []
  },
  "hooks": {
    "PostToolUse": [
      {
        "matcher": "Edit|Write",
        "hooks": [
          {
            "type": "command",
            "command": "cmd /c \"if exist build.bat (build.bat test) else (echo no build yet)\""
          }
        ]
      }
    ]
  }
}
```

- [ ] **Step 2: Commit**

```bash
git add .claude/settings.json
git commit -m "chore: claude permissions allowlist + post-edit build-test hook"
```

---

## Task 3: doctest, manifest, build.bat, first passing test (PR1)

**Files:**
- Create: `third_party/doctest.h` (downloaded), `Wind.manifest`, `build.bat`, `tests/test_main.cpp`, `tests/test_smoke.cpp`

- [ ] **Step 1: Vendor doctest**

```bash
curl -L -o third_party/doctest.h https://raw.githubusercontent.com/doctest/doctest/v2.4.11/doctest/doctest.h
```
Expected: a ~7000-line header at `third_party/doctest.h`.

- [ ] **Step 2: Write `Wind.manifest` (Per-Monitor-V2 DPI awareness)**

```xml
<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0">
  <application xmlns="urn:schemas-microsoft-com:asm.v3">
    <windowsSettings>
      <dpiAwareness xmlns="http://schemas.microsoft.com/SMI/2016/WindowsSettings">PerMonitorV2</dpiAwareness>
    </windowsSettings>
  </application>
</assembly>
```

- [ ] **Step 3: Write `build.bat`**

```bat
@echo off
setlocal enabledelayedexpansion
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (echo vswhere not found & exit /b 1)
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if "%VSPATH%"=="" (echo VC tools not found & exit /b 1)
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul

if /i "%1"=="test" goto :test

cl /nologo /std:c++17 /EHsc /O2 /W4 /DUNICODE /D_UNICODE ^
   src\transform.cpp src\zoom_controller.cpp src\tracker.cpp src\config.cpp ^
   src\magnifier_engine.cpp src\input_router.cpp src\tray.cpp src\main.cpp ^
   /Fe:Wind.exe ^
   /link Magnification.lib Dwmapi.lib user32.lib shell32.lib gdi32.lib ^
   /MANIFEST:EMBED /MANIFESTINPUT:Wind.manifest /SUBSYSTEM:WINDOWS
exit /b %errorlevel%

:test
cl /nologo /std:c++17 /EHsc /W4 /I third_party ^
   tests\test_main.cpp tests\test_smoke.cpp tests\test_transform.cpp ^
   tests\test_zoom_controller.cpp tests\test_tracker.cpp tests\test_config.cpp ^
   src\transform.cpp src\zoom_controller.cpp src\tracker.cpp src\config.cpp ^
   /Fe:wind_tests.exe
if errorlevel 1 exit /b 1
wind_tests.exe
exit /b %errorlevel%
```
Note: the `test` target compiles only the pure-logic `.cpp` files (no Win32 sources), so tests build and run without a desktop session.

- [ ] **Step 4: Write `tests/test_main.cpp`**

```cpp
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
```

- [ ] **Step 5: Write a smoke test so the loop is provably alive**

`tests/test_smoke.cpp`:
```cpp
#include "doctest.h"
TEST_CASE("test harness runs") {
    CHECK(1 + 1 == 2);
}
```

- [ ] **Step 6: Create empty placeholder pure sources so the test target links**

Create `src/transform.cpp`, `src/zoom_controller.cpp`, `src/tracker.cpp`, `src/config.cpp`, each containing only:
```cpp
// implemented in later tasks
```

- [ ] **Step 7: Run the test target**

Run: `build.bat test`
Expected: compiles, runs `wind_tests.exe`, output shows `test harness runs` passing, exit code 0.

- [ ] **Step 8: Commit and open PR1**

```bash
git add third_party/doctest.h Wind.manifest build.bat tests/ src/
git commit -m "build: doctest harness, build.bat, DPI manifest, smoke test"
gh pr create --fill --base main
```

---

## Task 4: `transform` — center+level+screen → offset (PR2, TDD)

**Files:**
- Create: `src/transform.h`, `tests/test_transform.cpp`
- Modify: `src/transform.cpp`

- [ ] **Step 1: Branch + issue**

```bash
gh issue create --title "PR2: pure logic modules" --body "transform, zoom_controller, tracker, config parse — TDD."
git checkout main && git pull && git checkout -b feat/pure-logic
```

- [ ] **Step 2: Write the failing test** (`tests/test_transform.cpp`)

```cpp
#include "doctest.h"
#include "../src/transform.h"

TEST_CASE("centers the view at 2x") {
    wind::Offset o = wind::ComputeOffset(960, 540, 2.0, 1920, 1080);
    CHECK(o.x == 480);
    CHECK(o.y == 270);
}
TEST_CASE("clamps to top-left edge") {
    wind::Offset o = wind::ComputeOffset(0, 0, 2.0, 1920, 1080);
    CHECK(o.x == 0);
    CHECK(o.y == 0);
}
TEST_CASE("clamps to bottom-right edge") {
    wind::Offset o = wind::ComputeOffset(1920, 1080, 2.0, 1920, 1080);
    CHECK(o.x == 960);
    CHECK(o.y == 540);
}
TEST_CASE("level 1.0 always offsets to origin") {
    wind::Offset o = wind::ComputeOffset(500, 500, 1.0, 1920, 1080);
    CHECK(o.x == 0);
    CHECK(o.y == 0);
}
```

- [ ] **Step 3: Write the header** (`src/transform.h`)

```cpp
#pragma once
namespace wind {
struct Offset { int x; int y; };
// center: virtual lens center in screen pixels. level >= 1.0.
// screenW/H: monitor size in pixels. Returns top-left of the magnified source
// region, clamped so the view stays on screen.
Offset ComputeOffset(double centerX, double centerY, double level, int screenW, int screenH);
}
```

- [ ] **Step 4: Run the test to verify it fails**

Run: `build.bat test`
Expected: FAIL (linker error: `ComputeOffset` unresolved).

- [ ] **Step 5: Implement** (`src/transform.cpp`)

```cpp
#include "transform.h"
#include <algorithm>
#include <cmath>
namespace wind {
Offset ComputeOffset(double centerX, double centerY, double level, int screenW, int screenH) {
    if (level < 1.0) level = 1.0;
    double viewW = screenW / level;
    double viewH = screenH / level;
    int x = static_cast<int>(std::lround(centerX - viewW / 2.0));
    int y = static_cast<int>(std::lround(centerY - viewH / 2.0));
    int maxX = screenW - static_cast<int>(std::lround(viewW));
    int maxY = screenH - static_cast<int>(std::lround(viewH));
    x = std::min(std::max(x, 0), std::max(maxX, 0));
    y = std::min(std::max(y, 0), std::max(maxY, 0));
    return Offset{ x, y };
}
}
```

- [ ] **Step 6: Run the test to verify it passes**

Run: `build.bat test`
Expected: PASS, all `transform` cases green.

- [ ] **Step 7: Commit**

```bash
git add src/transform.h src/transform.cpp tests/test_transform.cpp
git commit -m "feat: transform — center/level/screen to clamped magnifier offset"
```

---

## Task 5: `zoom_controller` — ramp + direction resolution (PR2, TDD)

**Files:**
- Create: `src/zoom_controller.h`, `tests/test_zoom_controller.cpp`
- Modify: `src/zoom_controller.cpp`

- [ ] **Step 1: Write the failing test** (`tests/test_zoom_controller.cpp`)

```cpp
#include "doctest.h"
#include "../src/zoom_controller.h"
using namespace wind;

TEST_CASE("ResolveDirection maps physical button state") {
    CHECK(ResolveDirection(false, false) == ZoomDir::None);
    CHECK(ResolveDirection(true,  false) == ZoomDir::In);
    CHECK(ResolveDirection(false, true ) == ZoomDir::Out);
    CHECK(ResolveDirection(true,  true ) == ZoomDir::None); // both held = freeze
}
TEST_CASE("ramps in to max over full-range seconds") {
    ZoomController z(1.0, 8.0, 1.2);
    z.setDirection(ZoomDir::In);
    z.tick(1.2);
    CHECK(z.level() == doctest::Approx(8.0));
}
TEST_CASE("ramps out to min") {
    ZoomController z(1.0, 8.0, 1.2);
    z.setDirection(ZoomDir::In);  z.tick(1.2);   // at 8.0
    z.setDirection(ZoomDir::Out); z.tick(1.2);
    CHECK(z.level() == doctest::Approx(1.0));
}
TEST_CASE("half the time gives multiplicative midpoint") {
    ZoomController z(1.0, 8.0, 1.2);
    z.setDirection(ZoomDir::In);
    z.tick(0.6);
    CHECK(z.level() == doctest::Approx(2.8284).epsilon(0.001));
}
TEST_CASE("freezes when direction is None") {
    ZoomController z(1.0, 8.0, 1.2);
    z.setDirection(ZoomDir::In); z.tick(0.3);
    double held = z.level();
    z.setDirection(ZoomDir::None); z.tick(5.0);
    CHECK(z.level() == doctest::Approx(held));
}
TEST_CASE("clamps and never exceeds bounds with many small ticks") {
    ZoomController z(1.0, 8.0, 1.2);
    z.setDirection(ZoomDir::In);
    for (int i = 0; i < 1000; ++i) z.tick(0.01);
    CHECK(z.level() == doctest::Approx(8.0));
}
TEST_CASE("reset returns to 1.0 and None") {
    ZoomController z(1.0, 8.0, 1.2);
    z.setDirection(ZoomDir::In); z.tick(0.5);
    z.reset();
    CHECK(z.level() == doctest::Approx(1.0));
    CHECK(z.direction() == ZoomDir::None);
}
```

- [ ] **Step 2: Write the header** (`src/zoom_controller.h`)

```cpp
#pragma once
namespace wind {
enum class ZoomDir { None, In, Out };

// Pure: given which side buttons are physically held, what should the zoom do.
// Both held is ambiguous, so freeze.
ZoomDir ResolveDirection(bool inHeld, bool outHeld);

class ZoomController {
public:
    ZoomController(double minLevel, double maxLevel, double fullRangeSeconds);
    void setDirection(ZoomDir d);
    ZoomDir direction() const { return dir_; }
    void tick(double dtSeconds);   // ramp level multiplicatively toward bound
    double level() const { return level_; }
    void reset();                  // level=min, dir=None
private:
    double minLevel_, maxLevel_, fullRangeSeconds_;
    double level_;
    ZoomDir dir_ = ZoomDir::None;
};
}
```

- [ ] **Step 3: Run the test to verify it fails**

Run: `build.bat test`
Expected: FAIL (unresolved `ResolveDirection` / `ZoomController`).

- [ ] **Step 4: Implement** (`src/zoom_controller.cpp`)

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
void ZoomController::tick(double dt) {
    if (dir_ == ZoomDir::None || dt <= 0.0) return;
    double f = std::pow(maxLevel_ / minLevel_, dt / fullRangeSeconds_);
    if (dir_ == ZoomDir::In)  level_ *= f;
    else                       level_ /= f;
    level_ = std::min(maxLevel_, std::max(minLevel_, level_));
}
void ZoomController::reset() { level_ = minLevel_; dir_ = ZoomDir::None; }
}
```

- [ ] **Step 5: Run the test to verify it passes**

Run: `build.bat test`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add src/zoom_controller.h src/zoom_controller.cpp tests/test_zoom_controller.cpp
git commit -m "feat: zoom_controller — multiplicative hold-to-zoom ramp + direction resolve"
```

---

## Task 6: `tracker` — free/locked blend with delta integration (PR2, TDD)

**Files:**
- Create: `src/tracker.h`, `tests/test_tracker.cpp`
- Modify: `src/tracker.cpp`

- [ ] **Step 1: Write the failing test** (`tests/test_tracker.cpp`)

```cpp
#include "doctest.h"
#include "../src/tracker.h"
using namespace wind;

TEST_CASE("free mode follows the OS cursor when it moves") {
    Tracker t(1920, 1080, 1.0);
    t.update(100, 100, 0, 0);
    CHECK(t.centerX() == doctest::Approx(100));
    CHECK(t.centerY() == doctest::Approx(100));
    t.update(300, 200, 0, 0);
    CHECK(t.centerX() == doctest::Approx(300));
    CHECK(t.centerY() == doctest::Approx(200));
}
TEST_CASE("locked mode integrates raw deltas when cursor is frozen") {
    Tracker t(1920, 1080, 1.0);
    t.update(960, 540, 0, 0);          // establish position
    t.update(960, 540, 5, -3);         // cursor frozen, raw movement arrives
    CHECK(t.centerX() == doctest::Approx(965));
    CHECK(t.centerY() == doctest::Approx(537));
}
TEST_CASE("sensitivity scales locked-mode panning") {
    Tracker t(1920, 1080, 2.0);
    t.update(960, 540, 0, 0);
    t.update(960, 540, 10, 0);
    CHECK(t.centerX() == doctest::Approx(980)); // 10 * 2.0
}
TEST_CASE("returns to free mode and snaps to cursor when it moves again") {
    Tracker t(1920, 1080, 1.0);
    t.update(960, 540, 0, 0);
    t.update(960, 540, 50, 0);          // locked -> 1010
    t.update(400, 400, 0, 0);           // cursor moved -> free, snap
    CHECK(t.centerX() == doctest::Approx(400));
    CHECK(t.centerY() == doctest::Approx(400));
}
TEST_CASE("holds still when neither cursor nor raw move") {
    Tracker t(1920, 1080, 1.0);
    t.update(700, 700, 0, 0);
    t.update(700, 700, 0, 0);
    CHECK(t.centerX() == doctest::Approx(700));
}
TEST_CASE("clamps center to screen bounds in locked mode") {
    Tracker t(1920, 1080, 1.0);
    t.update(10, 10, 0, 0);
    t.update(10, 10, -1000, -1000);
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

- [ ] **Step 2: Write the header** (`src/tracker.h`)

```cpp
#pragma once
namespace wind {
// Pure tracking state. Caller samples GetCursorPos and the summed raw-input deltas
// since the last tick, then calls update() once per tick.
class Tracker {
public:
    Tracker(int screenW, int screenH, double sensitivity);
    // cursorX/Y: latest GetCursorPos. rawDx/Dy: summed WM_INPUT deltas since last call.
    void update(int cursorX, int cursorY, int rawDx, int rawDy);
    void recenter();                 // snap to screen center
    double centerX() const { return cx_; }
    double centerY() const { return cy_; }
private:
    void clamp();
    int screenW_, screenH_;
    double sensitivity_;
    double cx_, cy_;
    int lastCursorX_, lastCursorY_;
    bool haveCursor_ = false;
};
}
```

- [ ] **Step 3: Run the test to verify it fails**

Run: `build.bat test`
Expected: FAIL (unresolved `Tracker`).

- [ ] **Step 4: Implement** (`src/tracker.cpp`)

```cpp
#include "tracker.h"
#include <algorithm>
namespace wind {
Tracker::Tracker(int screenW, int screenH, double sensitivity)
    : screenW_(screenW), screenH_(screenH), sensitivity_(sensitivity),
      cx_(screenW / 2.0), cy_(screenH / 2.0),
      lastCursorX_(0), lastCursorY_(0) {}

void Tracker::clamp() {
    cx_ = std::min(std::max(cx_, 0.0), static_cast<double>(screenW_));
    cy_ = std::min(std::max(cy_, 0.0), static_cast<double>(screenH_));
}

void Tracker::update(int cursorX, int cursorY, int rawDx, int rawDy) {
    bool cursorMoved = !haveCursor_ || cursorX != lastCursorX_ || cursorY != lastCursorY_;
    if (cursorMoved) {
        cx_ = cursorX;            // free mode: follow OS cursor
        cy_ = cursorY;
    } else if (rawDx != 0 || rawDy != 0) {
        cx_ += rawDx * sensitivity_;   // locked mode: integrate raw movement
        cy_ += rawDy * sensitivity_;
    }
    clamp();
    lastCursorX_ = cursorX;
    lastCursorY_ = cursorY;
    haveCursor_ = true;
}

void Tracker::recenter() {
    cx_ = screenW_ / 2.0;
    cy_ = screenH_ / 2.0;
}
}
```

- [ ] **Step 5: Run the test to verify it passes**

Run: `build.bat test`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add src/tracker.h src/tracker.cpp tests/test_tracker.cpp
git commit -m "feat: tracker — free/locked blend with raw-delta integration and clamp"
```

---

## Task 7: `config` — parse (pure) + load/save/hot-reload (PR2 pure half, PR3 I/O half)

**Files:**
- Create: `src/config.h`, `tests/test_config.cpp`
- Modify: `src/config.cpp`

- [ ] **Step 1: Write the failing test** (`tests/test_config.cpp`) — pure parse only

```cpp
#include "doctest.h"
#include "../src/config.h"
using namespace wind;

TEST_CASE("defaults when text is empty") {
    Config c = ParseConfig("");
    CHECK(c.zoomInButton  == 2);   // XBUTTON2
    CHECK(c.zoomOutButton == 1);   // XBUTTON1
    CHECK(c.maxLevel == doctest::Approx(8.0));
    CHECK(c.fullRangeSeconds == doctest::Approx(1.2));
    CHECK(c.sensitivity == doctest::Approx(1.0));
}
TEST_CASE("parses overrides and ignores comments/blank lines") {
    const char* ini =
        "; comment\n"
        "maxLevel = 12.5\n"
        "\n"
        "zoomInButton=1\n"
        "sensitivity = 0.5\n";
    Config c = ParseConfig(ini);
    CHECK(c.maxLevel == doctest::Approx(12.5));
    CHECK(c.zoomInButton == 1);
    CHECK(c.sensitivity == doctest::Approx(0.5));
    CHECK(c.fullRangeSeconds == doctest::Approx(1.2)); // untouched default
}
TEST_CASE("malformed lines are ignored, keep defaults") {
    Config c = ParseConfig("garbage line\nmaxLevel\n=5\n");
    CHECK(c.maxLevel == doctest::Approx(8.0));
}
```

- [ ] **Step 2: Write the header** (`src/config.h`)

```cpp
#pragma once
#include <string>
namespace wind {
struct Config {
    int    zoomInButton    = 2;     // 1 = XBUTTON1 (back), 2 = XBUTTON2 (forward)
    int    zoomOutButton   = 1;
    int    recenterVk      = 0;     // 0 = unbound
    double maxLevel        = 8.0;
    double fullRangeSeconds = 1.2;
    double sensitivity     = 1.0;
    int    tickHzCap       = 144;
};
// Pure: parse INI text (key=value, ';' or '#' comments) into a Config, keeping
// defaults for missing/malformed keys.
Config ParseConfig(const std::string& text);

// I/O (implemented for PR3): read file -> ParseConfig; create with defaults if absent.
Config LoadConfig(const std::wstring& path);
// I/O: last write time as a comparable tick count; 0 if missing.
unsigned long long ConfigMTime(const std::wstring& path);
}
```

- [ ] **Step 3: Run the test to verify it fails**

Run: `build.bat test`
Expected: FAIL (unresolved `ParseConfig`).

- [ ] **Step 4: Implement the pure parser** (`src/config.cpp`)

```cpp
#include "config.h"
#include <sstream>
#include <string>
namespace wind {
static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}
Config ParseConfig(const std::string& text) {
    Config c;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        std::string t = trim(line);
        if (t.empty() || t[0] == ';' || t[0] == '#') continue;
        size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(t.substr(0, eq));
        std::string val = trim(t.substr(eq + 1));
        if (key.empty() || val.empty()) continue;
        try {
            if (key == "zoomInButton")        c.zoomInButton = std::stoi(val);
            else if (key == "zoomOutButton")  c.zoomOutButton = std::stoi(val);
            else if (key == "recenterVk")     c.recenterVk = std::stoi(val);
            else if (key == "maxLevel")       c.maxLevel = std::stod(val);
            else if (key == "fullRangeSeconds") c.fullRangeSeconds = std::stod(val);
            else if (key == "sensitivity")    c.sensitivity = std::stod(val);
            else if (key == "tickHzCap")      c.tickHzCap = std::stoi(val);
        } catch (...) { /* keep default on bad value */ }
    }
    return c;
}
}
```
(The `LoadConfig`/`ConfigMTime` I/O functions are added in Task 10; they are not part of the test build.)

- [ ] **Step 5: Run the test to verify it passes**

Run: `build.bat test`
Expected: PASS. This completes PR2.

- [ ] **Step 6: Commit + open PR2**

```bash
git add src/config.h src/config.cpp tests/test_config.cpp
git commit -m "feat: config — pure INI parser with defaults"
gh pr create --fill --base main
```

---

## Task 8: `magnifier_engine` — Magnification API wrapper (PR3)

**Files:**
- Create: `src/magnifier_engine.h`, `src/magnifier_engine.cpp`

This module is Win32 I/O; it is verified by the manual checklist in Task 13, not unit tests.

- [ ] **Step 1: Branch + issue**

```bash
gh issue create --title "PR3: I/O modules" --body "Magnification engine, input router, config file I/O."
git checkout main && git pull && git checkout -b feat/io-shell
```

- [ ] **Step 2: Write the header** (`src/magnifier_engine.h`)

```cpp
#pragma once
namespace wind {
class MagnifierEngine {
public:
    bool initialize();                                  // MagInitialize
    void setTransform(double level, int xOffset, int yOffset);
    void shutdown();                                    // reset to 1x then MagUninitialize
    bool ready() const { return ready_; }
private:
    bool ready_ = false;
};
}
```

- [ ] **Step 3: Implement** (`src/magnifier_engine.cpp`)

```cpp
#include "magnifier_engine.h"
#include <windows.h>
#include <magnification.h>
namespace wind {
bool MagnifierEngine::initialize() {
    ready_ = MagInitialize() ? true : false;
    return ready_;
}
void MagnifierEngine::setTransform(double level, int xOffset, int yOffset) {
    if (!ready_) return;
    MagSetFullscreenTransform(static_cast<float>(level), xOffset, yOffset);
}
void MagnifierEngine::shutdown() {
    if (!ready_) return;
    MagSetFullscreenTransform(1.0f, 0, 0);  // never leave the screen zoomed
    MagUninitialize();
    ready_ = false;
}
}
```

- [ ] **Step 4: Build the app to confirm it compiles/links**

Run: `build.bat`
Expected: `Wind.exe` builds (it won't do anything useful until `main` is wired in Task 11). If `main.cpp`/other sources don't exist yet, temporarily allow the build to fail on those only; this step just checks `magnifier_engine` compiles. (Alternatively defer this build check to Task 11.)

- [ ] **Step 5: Commit**

```bash
git add src/magnifier_engine.h src/magnifier_engine.cpp
git commit -m "feat: magnifier_engine — Magnification API fullscreen-transform wrapper"
```

---

## Task 9: `input_router` — Raw Input + low-level mouse hook (PR3)

**Files:**
- Create: `src/input_router.h`, `src/input_router.cpp`

Captures mouse-movement deltas via Raw Input (survives cursor lock) and side-button
state via `WH_MOUSE_LL` (so the buttons can be swallowed and don't fire browser
back/forward). Shared state is exposed as atomics the tick thread reads.

- [ ] **Step 1: Write the header** (`src/input_router.h`)

```cpp
#pragma once
#include <atomic>
namespace wind {
// Holds input state shared between the hook/raw-input callbacks and the tick thread.
struct InputState {
    std::atomic<int>  rawDx{0};      // summed since last drain
    std::atomic<int>  rawDy{0};
    std::atomic<bool> inHeld{false}; // zoom-in side button physically down
    std::atomic<bool> outHeld{false};
    std::atomic<bool> recenter{false};
};

class InputRouter {
public:
    // inButtonId/outButtonId: 1 = XBUTTON1, 2 = XBUTTON2. swallow: block the buttons
    // from reaching other apps while running.
    bool start(int inButtonId, int outButtonId, bool swallow);
    void stop();
    InputState& state() { return state_; }
    // Atomically read and zero the accumulated raw deltas.
    void drainRaw(int& dx, int& dy);
private:
    InputState state_;
};
}
```

- [ ] **Step 2: Implement** (`src/input_router.cpp`)

```cpp
#include "input_router.h"
#include <windows.h>
namespace wind {
static InputRouter* g_router = nullptr;
static int  g_inButtonId = 2, g_outButtonId = 1;
static bool g_swallow = true;
static HHOOK g_mouseHook = nullptr;

static int xbuttonIdFromHook(WPARAM wParam, LPARAM lParam) {
    auto* mi = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);
    if (wParam == WM_XBUTTONDOWN || wParam == WM_XBUTTONUP) {
        WORD hi = HIWORD(mi->mouseData); // XBUTTON1 or XBUTTON2
        return (hi == XBUTTON1) ? 1 : (hi == XBUTTON2 ? 2 : 0);
    }
    return 0;
}

static LRESULT CALLBACK MouseProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION && g_router) {
        int id = xbuttonIdFromHook(wParam, lParam);
        bool down = (wParam == WM_XBUTTONDOWN);
        bool up   = (wParam == WM_XBUTTONUP);
        if (id != 0 && (down || up)) {
            if (id == g_inButtonId)  g_router->state().inHeld.store(down);
            if (id == g_outButtonId) g_router->state().outHeld.store(down);
            if (g_swallow && (id == g_inButtonId || id == g_outButtonId))
                return 1; // swallow so back/forward don't fire
        }
    }
    return CallNextHookEx(g_mouseHook, code, wParam, lParam);
}

bool InputRouter::start(int inButtonId, int outButtonId, bool swallow) {
    g_router = this; g_inButtonId = inButtonId; g_outButtonId = outButtonId; g_swallow = swallow;
    g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, MouseProc, GetModuleHandleW(nullptr), 0);
    return g_mouseHook != nullptr;
    // NOTE: Raw Input registration (RIDEV_INPUTSINK) and WM_INPUT handling live in
    // main.cpp's message-only window, which calls back into accumulateRaw().
}
void InputRouter::stop() {
    if (g_mouseHook) { UnhookWindowsHookEx(g_mouseHook); g_mouseHook = nullptr; }
    g_router = nullptr;
}
void InputRouter::drainRaw(int& dx, int& dy) {
    dx = state_.rawDx.exchange(0);
    dy = state_.rawDy.exchange(0);
}
}
```

- [ ] **Step 3: Add a helper main.cpp will call from WM_INPUT** — append to `input_router.cpp`

```cpp
namespace wind {
// Called from main.cpp's WM_INPUT handler with decoded relative deltas.
void AccumulateRaw(InputRouter& r, int dx, int dy) {
    r.state().rawDx.fetch_add(dx);
    r.state().rawDy.fetch_add(dy);
}
}
```
And declare it in `input_router.h`:
```cpp
namespace wind { class InputRouter; void AccumulateRaw(InputRouter& r, int dx, int dy); }
```

- [ ] **Step 4: Commit**

```bash
git add src/input_router.h src/input_router.cpp
git commit -m "feat: input_router — WH_MOUSE_LL side-button capture + raw-delta accumulator"
```

---

## Task 10: `config` file I/O + hot-reload helpers (PR3)

**Files:**
- Modify: `src/config.cpp` (add `LoadConfig`, `ConfigMTime`)

- [ ] **Step 1: Append I/O functions** to `src/config.cpp`

```cpp
#include <windows.h>
#include <fstream>
namespace wind {
Config LoadConfig(const std::wstring& path) {
    std::ifstream f(path);
    if (!f) {
        // Write defaults so the user has something to edit.
        std::ofstream out(path);
        out << "; Wind magnifier config\n"
               "zoomInButton=2\nzoomOutButton=1\nrecenterVk=0\n"
               "maxLevel=8.0\nfullRangeSeconds=1.2\nsensitivity=1.0\ntickHzCap=144\n";
        return Config{};
    }
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return ParseConfig(text);
}
unsigned long long ConfigMTime(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &d)) return 0ULL;
    ULARGE_INTEGER u; u.LowPart = d.ftLastWriteTime.dwLowDateTime;
    u.HighPart = d.ftLastWriteTime.dwHighDateTime;
    return u.QuadPart;
}
}
```
Note: these use `<windows.h>` but live in the same `config.cpp`. Keep them **below** the pure parser and guarded so the test build excludes them: wrap this block in `#ifndef WIND_TESTS ... #endif`, and add `/DWIND_TESTS` to the `:test` `cl` line in `build.bat`.

- [ ] **Step 2: Add the guard** — wrap the Step 1 block:

```cpp
#ifndef WIND_TESTS
// ... the LoadConfig / ConfigMTime block ...
#endif
```
And in `build.bat` `:test`, add `/DWIND_TESTS` to the `cl` flags.

- [ ] **Step 3: Run tests to confirm the pure build still passes**

Run: `build.bat test`
Expected: PASS (I/O block excluded by `WIND_TESTS`).

- [ ] **Step 4: Commit + open PR3**

```bash
git add src/config.cpp build.bat
git commit -m "feat: config file load (with default write) + mtime for hot-reload"
gh pr create --fill --base main
```

---

## Task 11: `main.cpp` — wire everything, tick thread, lifecycle (PR4)

**Files:**
- Create: `src/main.cpp`

- [ ] **Step 1: Branch + issue**

```bash
gh issue create --title "PR4: wiring, tray, verification" --body "main loop, tick thread, tray icon, manual verification + tuning."
git checkout main && git pull && git checkout -b feat/wiring
```

- [ ] **Step 2: Implement** (`src/main.cpp`)

```cpp
#include <windows.h>
#include <dwmapi.h>
#include <thread>
#include <atomic>
#include "config.h"
#include "magnifier_engine.h"
#include "input_router.h"
#include "transform.h"
#include "tracker.h"
#include "zoom_controller.h"
#include "tray.h"

using namespace wind;

static InputRouter g_input;
static std::atomic<bool> g_running{true};

// Message-only window receives WM_INPUT (raw mouse) and tray messages.
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_INPUT) {
        UINT size = 0;
        GetRawInputData((HRAWINPUT)lp, RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER));
        BYTE buf[64];
        if (size <= sizeof(buf) &&
            GetRawInputData((HRAWINPUT)lp, RID_INPUT, buf, &size, sizeof(RAWINPUTHEADER)) == size) {
            auto* ri = reinterpret_cast<RAWINPUT*>(buf);
            if (ri->header.dwType == RIM_TYPEMOUSE &&
                (ri->data.mouse.usFlags & MOUSE_MOVE_RELATIVE) == MOUSE_MOVE_RELATIVE) {
                AccumulateRaw(g_input, ri->data.mouse.lLastX, ri->data.mouse.lLastY);
            }
        }
        return 0;
    }
    if (Tray::HandleMessage(hwnd, msg, wp, lp)) return 0; // tray menu/quit
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
    // Single instance
    HANDLE mtx = CreateMutexW(nullptr, TRUE, L"Wind_Magnifier_SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    Config cfg = LoadConfig(L"magnifier.ini");

    // Message-only window
    WNDCLASSW wc{}; wc.lpfnWndProc = WndProc; wc.hInstance = hInst;
    wc.lpszClassName = L"WindMagnifierWnd";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"Wind", 0, 0, 0, 0, 0,
                              HWND_MESSAGE, nullptr, hInst, nullptr);

    // Raw Input for the mouse, delivered even when a game is foreground.
    RAWINPUTDEVICE rid{}; rid.usUsagePage = 0x01; rid.usUsage = 0x02; // generic mouse
    rid.dwFlags = RIDEV_INPUTSINK; rid.hwndTarget = hwnd;
    RegisterRawInputDevices(&rid, 1, sizeof(rid));

    if (!g_input.start(cfg.zoomInButton, cfg.zoomOutButton, /*swallow=*/true)) return 1;

    MagnifierEngine engine;
    if (!engine.initialize()) {
        Tray::Notify(L"Wind", L"Magnification API failed to initialize.");
        return 1;
    }
    Tray::Add(hwnd, hInst);

    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    ZoomController zoom(1.0, cfg.maxLevel, cfg.fullRangeSeconds);
    Tracker tracker(sw, sh, cfg.sensitivity);

    // Tick thread: paced by DwmFlush (≈ refresh rate), reads shared state, transforms.
    std::thread tick([&]{
        LARGE_INTEGER freq, prev; QueryPerformanceFrequency(&freq); QueryPerformanceCounter(&prev);
        while (g_running.load()) {
            DwmFlush(); // wait for next compositor frame (cheap pacing)
            LARGE_INTEGER now; QueryPerformanceCounter(&now);
            double dt = double(now.QuadPart - prev.QuadPart) / double(freq.QuadPart);
            prev = now;

            // Watchdog-safe: derive direction from current physical button state each tick.
            zoom.setDirection(ResolveDirection(g_input.state().inHeld.load(),
                                               g_input.state().outHeld.load()));
            zoom.tick(dt);

            if (g_input.state().recenter.exchange(false)) tracker.recenter();

            POINT p; GetCursorPos(&p);
            int dx, dy; g_input.drainRaw(dx, dy);
            tracker.update(p.x, p.y, dx, dy);

            Offset o = ComputeOffset(tracker.centerX(), tracker.centerY(),
                                     zoom.level(), sw, sh);
            engine.setTransform(zoom.level(), o.x, o.y);
        }
    });

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) { TranslateMessage(&msg); DispatchMessageW(&msg); }

    g_running.store(false);
    if (tick.joinable()) tick.join();
    engine.shutdown();       // resets to 1x — never leave the screen zoomed
    g_input.stop();
    Tray::Remove();
    ReleaseMutex(mtx);
    return 0;
}
```

- [ ] **Step 3: Build**

Run: `build.bat`
Expected: `Wind.exe` builds and links (Task 12 provides `tray.*`; if building before Task 12, stub `tray.h`/`tray.cpp` first — see Task 12).

- [ ] **Step 4: Commit**

```bash
git add src/main.cpp
git commit -m "feat: main — wire modules, raw-input window, DwmFlush tick thread, lifecycle"
```

---

## Task 12: `tray` — system tray icon + menu (PR4)

**Files:**
- Create: `src/tray.h`, `src/tray.cpp`

- [ ] **Step 1: Write the header** (`src/tray.h`)

```cpp
#pragma once
#include <windows.h>
namespace wind {
namespace Tray {
    void Add(HWND hwnd, HINSTANCE hInst);          // add icon
    void Remove();                                 // delete icon
    void Notify(const wchar_t* title, const wchar_t* text); // balloon
    bool HandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp); // true if handled
}
}
```

- [ ] **Step 2: Implement** (`src/tray.cpp`)

```cpp
#include "tray.h"
#include <shellapi.h>
namespace wind { namespace Tray {
static NOTIFYICONDATAW g_nid{};
static const UINT WM_TRAY = WM_APP + 1;
static const UINT ID_EDIT = 1001, ID_QUIT = 1002;

void Add(HWND hwnd, HINSTANCE hInst) {
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    lstrcpyW(g_nid.szTip, L"Wind magnifier");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}
void Remove() { Shell_NotifyIconW(NIM_DELETE, &g_nid); }
void Notify(const wchar_t* title, const wchar_t* text) {
    g_nid.uFlags = NIF_INFO;
    lstrcpyW(g_nid.szInfoTitle, title);
    lstrcpyW(g_nid.szInfo, text);
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
}
bool HandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_TRAY && (lp == WM_RBUTTONUP || lp == WM_LBUTTONUP)) {
        POINT pt; GetCursorPos(&pt);
        HMENU m = CreatePopupMenu();
        AppendMenuW(m, MF_STRING, ID_EDIT, L"Edit config");
        AppendMenuW(m, MF_STRING, ID_QUIT, L"Quit");
        SetForegroundWindow(hwnd);
        int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
        DestroyMenu(m);
        if (cmd == ID_EDIT) ShellExecuteW(nullptr, L"open", L"notepad.exe", L"magnifier.ini", nullptr, SW_SHOW);
        else if (cmd == ID_QUIT) PostMessageW(hwnd, WM_CLOSE, 0, 0);
        return true;
    }
    if (msg == WM_CLOSE) { DestroyWindow(hwnd); return true; }
    if (msg == WM_DESTROY) { PostQuitMessage(0); return true; }
    return false;
}
}}
```

- [ ] **Step 3: Build the full app**

Run: `build.bat`
Expected: `Wind.exe` builds and links cleanly at `/W4`.

- [ ] **Step 4: Commit**

```bash
git add src/tray.h src/tray.cpp
git commit -m "feat: tray — icon, right-click menu (edit config / quit), balloon"
```

---

## Task 13: Manual verification, tuning, hot-reload, PR4 close (PR4)

**Files:**
- Create: `docs/VERIFICATION.md`
- Modify: `src/main.cpp` (config hot-reload poll), `config/magnifier.ini`

- [ ] **Step 1: Add config hot-reload to the tick thread** — in `main.cpp`, before the tick loop add an mtime poll roughly once per second and re-apply tunable fields:

```cpp
// inside the tick lambda, after computing dt:
static unsigned long long lastMtime = ConfigMTime(L"magnifier.ini");
static double sinceCheck = 0; sinceCheck += dt;
if (sinceCheck > 1.0) {
    sinceCheck = 0;
    unsigned long long m = ConfigMTime(L"magnifier.ini");
    if (m != lastMtime) {
        lastMtime = m;
        Config nc = LoadConfig(L"magnifier.ini");
        zoom = ZoomController(1.0, nc.maxLevel, nc.fullRangeSeconds);
        tracker = Tracker(sw, sh, nc.sensitivity);
    }
}
```
(Capture `zoom`/`tracker` by reference in the lambda — they already are.)

- [ ] **Step 2: Ship a default `config/magnifier.ini`**

```ini
; Wind magnifier config. Edit and save; changes apply within ~1s.
zoomInButton=2
zoomOutButton=1
recenterVk=0
maxLevel=8.0
fullRangeSeconds=1.2
sensitivity=1.0
tickHzCap=144
```

- [ ] **Step 3: Build**

Run: `build.bat`
Expected: clean build.

- [ ] **Step 4: Write `docs/VERIFICATION.md` checklist and run it**

```markdown
# Wind manual verification

Run `Wind.exe`. Then verify:

## Desktop
- [ ] Hold forward (XButton2): screen zooms in smoothly (no steps), follows the cursor.
- [ ] Hold back (XButton1): zooms out smoothly; stops at 1.0x (screen back to normal).
- [ ] Release mid-zoom: level stays put.
- [ ] Move the mouse while zoomed: the lens follows the cursor.
- [ ] Quit from tray: screen returns to 1x (never left zoomed).
- [ ] Edit magnifier.ini (set maxLevel=4.0), save: new max applies within ~1s.

## In a borderless-fullscreen game (cursor hidden / center-locked)
- [ ] Hold forward: game view zooms in.
- [ ] Move mouse: the lens PANS even though the game hides/locks the cursor (core feature).
- [ ] Side buttons do not trigger anything in-game unexpectedly.

## Performance
- [ ] Task Manager: Wind CPU stays near 0% idle-zoomed; low while panning.
- [ ] No stutter added to the game.
```

Run through the checklist manually; record results.

- [ ] **Step 5: Commit + open PR4**

```bash
git add docs/VERIFICATION.md config/magnifier.ini src/main.cpp
git commit -m "feat: config hot-reload, default ini, manual verification checklist"
gh pr create --fill --base main
```

---

## Self-review (completed by plan author)

**Spec coverage:**
- Fullscreen magnification → Task 8 (`MagSetFullscreenTransform`). ✓
- Responsive → DwmFlush-paced tick, Task 11. ✓
- Light → Magnification API (no per-frame copy) + atomic state; performance check in Task 13. ✓
- Smooth gradual zoom → multiplicative ramp, Task 5; float transform, Task 8. ✓
- Works in games / movable under cursor lock → Raw Input (Task 9, 11) + Tracker free/locked blend (Task 6). ✓
- Hold-to-zoom, forward=in / back=out, persist on release → `ResolveDirection` + `ZoomController` (Task 5), `WH_MOUSE_LL` capture (Task 9), polled each tick (Task 11). ✓
- Config / tray / single instance / DPI / graceful shutdown → Tasks 7, 10, 11, 12, 13 + manifest Task 3. ✓
- Verification loop day one → Task 3 smoke test + `build.bat test`. ✓
- Non-goals (exclusive fullscreen, injection) → explicitly excluded; documented in README/CLAUDE.md.

**Placeholder scan:** Empty pure-source files in Task 3 are intentional link stubs, replaced in Tasks 4-7. The Task 8 Step 4 partial-build note is a sequencing convenience, fully resolved by Task 12. No "TBD"/"add error handling"/unshown code remain.

**Type consistency:** `ComputeOffset`, `Offset`, `ZoomDir`, `ResolveDirection`, `ZoomController`, `Tracker`, `Config`, `ParseConfig`, `LoadConfig`, `ConfigMTime`, `MagnifierEngine`, `InputRouter`, `InputState`, `AccumulateRaw`, `Tray::*` are used with identical signatures across tasks and `main.cpp`. ✓
```
