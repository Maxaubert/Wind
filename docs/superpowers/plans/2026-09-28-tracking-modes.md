# Tracking modes Implementation Plan (issue #276)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The zoomed view can follow the text caret (on by default), keyboard focus (off by default), and (phase 2) an edge mode where the pointer roams freely and the view pans near the edges.

**Architecture:** A watcher thread (`focus_track`) owns all accessibility calls (WinEvents, `GetGUIThreadInfo`, UI Automation on an MTA thread) and publishes the latest caret/focus rectangle. RunTick steps a pure owner state machine (`view_target.h`), glides a detached view centre (`view_glide.h`) and builds the frame from `detached_view.h`, suppressing the weld so the pointer never moves. Phase 2 adds `edge_pan.h` for the mouse edge mode.

**Tech Stack:** C++17 / MSVC, Win32 (`SetWinEventHook`, `GetGUIThreadInfo`), UI Automation COM (`UIAutomation.h`; links `ole32.lib oleaut32.lib uuid.lib oleacc.lib`), doctest, Svelte + Playwright for Settings.

**Spec:** `docs/superpowers/specs/2026-09-28-tracking-modes-design.md`

## Global Constraints

- Tracking NEVER moves the pointer: while the owner is `Caret`, `Focus` or `Returning`, `ex.suppressCursorSync = true` and no `SetCursorPos` runs for tracking.
- Defaults: `trackCaret=1`, `trackFocus=0`, `trackAlign=0` (centred), `mouseAlign=0` (centred), `trackGlideMs=150`, `trackMarginPct=15`, `trackLog=0`.
- Caret and focus glide (95% of the distance in `trackGlideMs`), never snap; the return to the pointer glides too.
- Mouse takes the view back at 3 px of real movement accumulated within 100 ms, or any button down.
- Tracking inactive when not zoomed, in a game session, in Inspect, or while the lock detector reports locked.
- The tick thread never calls UIA/MSAA; only the watcher thread does, COM MTA.
- Pure headers include no `<windows.h>` and get doctests in `tests/`.
- No em-dashes anywhere (code, comments, docs, UI copy, commits).
- Versions: PR A `0.10.3`, PR B `0.10.4` (patch bumps, owner preference).
- Every repo change goes through a branch + PR; merge only after the owner approves that PR.

## Review Focus

1. Mouse sensor jitter while typing (1-2 px) must not steal the view: pinned in Task 2 (`jitter below threshold keeps Caret`).
2. A caret rect that is empty, (0,0,0,0), or outside the zoomed monitor must be ignored, never jump the view to the corner: pinned in Task 3 (`degenerate and off-monitor rects are rejected`).
3. Fast typing (a caret event every 30 ms) must glide continuously with no restart jolt: pinned in Task 3 (`retargeting mid-glide never moves backwards`).
4. Uneven tick intervals (VRR, 7-25 ms) must give the same glide in real time: pinned in Task 3 (`glide is time-based`).
5. A rect larger than the view in within-edges mode aligns its top-left instead of oscillating: pinned in Task 3 (`oversized rect aligns top-left`).

---

## Phase 1 (PR A): caret and focus tracking

### Task 1: Settings keys and defaults

**Files:**
- Modify: `src/config.h` (Config struct, near `cursorBandAuto`)
- Modify: `src/config.cpp` (ParseConfig keys, `DefaultIniText()` template)
- Test: `tests/test_config.cpp`

**Interfaces:**
- Produces: `Config::trackCaret` (int, 1), `trackFocus` (0), `trackAlign` (0 centred / 1 edges), `mouseAlign` (0 / 1), `trackGlideMs` (150), `trackMarginPct` (15), `trackLog` (0).

- [ ] **Step 1: Write the failing test** (append to `tests/test_config.cpp`)

```cpp
TEST_CASE("tracking settings: defaults and parsing (issue #276)") {
    Config d = ParseConfig("");
    CHECK(d.trackCaret == 1);
    CHECK(d.trackFocus == 0);
    CHECK(d.trackAlign == 0);
    CHECK(d.mouseAlign == 0);
    CHECK(d.trackGlideMs == 150);
    CHECK(d.trackMarginPct == 15);
    CHECK(d.trackLog == 0);
    Config c = ParseConfig("trackCaret=0\ntrackFocus=1\ntrackAlign=1\nmouseAlign=1\n"
                           "trackGlideMs=90\ntrackMarginPct=20\ntrackLog=1\n");
    CHECK(c.trackCaret == 0); CHECK(c.trackFocus == 1); CHECK(c.trackAlign == 1);
    CHECK(c.mouseAlign == 1); CHECK(c.trackGlideMs == 90); CHECK(c.trackMarginPct == 20);
    CHECK(c.trackLog == 1);
}
```

- [ ] **Step 2: Run** `build.bat test`. Expected: compile error, `trackCaret` is not a member.

- [ ] **Step 3: Implement.** In `Config` (config.h):

```cpp
    // Tracking modes (issue #276, hot). The view can follow the text caret and keyboard focus;
    // the pointer is never moved by tracking. Caret on by default, focus off.
    int trackCaret = 1;
    int trackFocus = 0;
    int trackAlign = 0;      // caret + focus: 0 = centred, 1 = within the edges
    int mouseAlign = 0;      // mouse: 0 = centred (today), 1 = within the edges (phase 2)
    int trackGlideMs = 150;  // glide time to 95% of the distance
    int trackMarginPct = 15; // within-edges margin, % of the view on each side
    int trackLog = 0;        // hidden: log every resolved caret/focus event with its source
```

In ParseConfig, next to `cursorBandAuto`:

```cpp
            else if (key == "trackCaret")         c.trackCaret = std::stoi(val);
            else if (key == "trackFocus")         c.trackFocus = std::stoi(val);
            else if (key == "trackAlign")         c.trackAlign = std::stoi(val);
            else if (key == "mouseAlign")         c.mouseAlign = std::stoi(val);
            else if (key == "trackGlideMs")       c.trackGlideMs = std::stoi(val);
            else if (key == "trackMarginPct")     c.trackMarginPct = std::stoi(val);
            else if (key == "trackLog")           c.trackLog = std::stoi(val);
```

In `DefaultIniText()`, after the `cursorBandAuto` block (trackLog stays out of the template):

```cpp
               "; trackCaret: 1=the zoomed view follows the text cursor while you type; 0=off\n"
               "trackCaret=1\n"
               "; trackFocus: 1=the zoomed view follows keyboard focus (Tab, menus); 0=off\n"
               "trackFocus=0\n"
               "; trackAlign: text cursor and focus, 0=keep centred, 1=keep within the edges\n"
               "trackAlign=0\n"
               "; mouseAlign: mouse pointer, 0=keep centred, 1=keep within the edges\n"
               "mouseAlign=0\n"
               "; trackGlideMs: how long the view takes to glide to the caret/focus/pointer (ms)\n"
               "trackGlideMs=150\n"
               "; trackMarginPct: within-the-edges margin, percent of the view on each side\n"
               "trackMarginPct=15\n"
```

- [ ] **Step 4: Run** `build.bat test`. Expected: PASS, including the generated "template parses to the struct defaults" test (it now covers the new keys).
- [ ] **Step 5: Commit** `feat(config): tracking mode settings (#276)`.

### Task 2: Owner state machine (`view_target.h`)

**Files:**
- Create: `src/view_target.h`
- Test: `tests/test_view_target.cpp`

**Interfaces:**
- Produces:

```cpp
namespace wind {
enum class ViewOwner { Mouse, Caret, Focus, Returning };
enum class TrackKind { None = 0, Caret = 1, Focus = 2 };
struct TrackSnapshot { TrackKind kind = TrackKind::None; unsigned seq = 0; double l = 0, t = 0, r = 0, b = 0; };
struct ViewOwnerState { ViewOwner owner = ViewOwner::Mouse; unsigned lastSeq = 0; double moveAccum = 0; double moveWindowMs = 0; };
struct ViewOwnerInputs {
    bool enabled = false;        // zoomed && !game && !inspect && !locked
    bool trackCaret = false, trackFocus = false;
    double mouseDx = 0, mouseDy = 0;   // real pointer movement this tick, px
    bool buttonDown = false;
    double dtMs = 0;
    TrackSnapshot snap;
};
ViewOwner StepViewOwner(ViewOwnerState& s, const ViewOwnerInputs& in);
void FinishReturn(ViewOwnerState& s);   // caller: the return glide arrived
inline constexpr double kMouseTakeoverPx = 3.0, kMouseTakeoverWindowMs = 100.0;
}
```

- [ ] **Step 1: Write the failing tests** (`tests/test_view_target.cpp`)

```cpp
#include "doctest.h"
#include "../src/view_target.h"
using namespace wind;

static ViewOwnerInputs Base() {
    ViewOwnerInputs in; in.enabled = true; in.trackCaret = true; in.trackFocus = true; in.dtMs = 7;
    return in;
}
static TrackSnapshot Snap(TrackKind k, unsigned seq) { TrackSnapshot s; s.kind = k; s.seq = seq; s.l = 100; s.t = 100; s.r = 102; s.b = 120; return s; }

TEST_CASE("a new caret event takes the view; a repeat of the same seq does not re-trigger") {
    ViewOwnerState s; auto in = Base(); in.snap = Snap(TrackKind::Caret, 1);
    CHECK(StepViewOwner(s, in) == ViewOwner::Caret);
    CHECK(StepViewOwner(s, in) == ViewOwner::Caret);
}
TEST_CASE("disabled kinds and disabled tracking leave the mouse in charge") {
    ViewOwnerState s; auto in = Base(); in.trackCaret = false; in.snap = Snap(TrackKind::Caret, 1);
    CHECK(StepViewOwner(s, in) == ViewOwner::Mouse);
    ViewOwnerState s2; auto in2 = Base(); in2.enabled = false; in2.snap = Snap(TrackKind::Focus, 1);
    CHECK(StepViewOwner(s2, in2) == ViewOwner::Mouse);
}
TEST_CASE("jitter below threshold keeps Caret") {
    ViewOwnerState s; auto in = Base(); in.snap = Snap(TrackKind::Caret, 1);
    StepViewOwner(s, in);
    in.mouseDx = 1; StepViewOwner(s, in);
    in.mouseDx = 1; CHECK(StepViewOwner(s, in) == ViewOwner::Caret);   // 2 px total
}
TEST_CASE("real mouse movement or a button starts the return") {
    ViewOwnerState s; auto in = Base(); in.snap = Snap(TrackKind::Caret, 1);
    StepViewOwner(s, in);
    in.mouseDx = 4; CHECK(StepViewOwner(s, in) == ViewOwner::Returning);
    FinishReturn(s); CHECK(s.owner == ViewOwner::Mouse);
    ViewOwnerState s2; auto in2 = Base(); in2.snap = Snap(TrackKind::Focus, 1);
    StepViewOwner(s2, in2);
    in2.buttonDown = true; CHECK(StepViewOwner(s2, in2) == ViewOwner::Returning);
}
TEST_CASE("slow drift spread over more than the window never accumulates to a takeover") {
    ViewOwnerState s; auto in = Base(); in.snap = Snap(TrackKind::Caret, 1);
    StepViewOwner(s, in);
    in.dtMs = 60;
    for (int i = 0; i < 10; ++i) { in.mouseDx = 1; CHECK(StepViewOwner(s, in) == ViewOwner::Caret); }
}
TEST_CASE("a caret event while returning takes the view again; mouse movement while Mouse stays Mouse") {
    ViewOwnerState s; auto in = Base(); in.snap = Snap(TrackKind::Caret, 1);
    StepViewOwner(s, in); in.mouseDx = 5; StepViewOwner(s, in);
    CHECK(s.owner == ViewOwner::Returning);
    in.mouseDx = 0; in.snap = Snap(TrackKind::Caret, 2);
    CHECK(StepViewOwner(s, in) == ViewOwner::Caret);
    ViewOwnerState m; auto mi = Base(); mi.mouseDx = 50;
    CHECK(StepViewOwner(m, mi) == ViewOwner::Mouse);
}
TEST_CASE("tracking turned off mid-caret goes straight back to the mouse") {
    ViewOwnerState s; auto in = Base(); in.snap = Snap(TrackKind::Caret, 1);
    StepViewOwner(s, in); in.enabled = false;
    CHECK(StepViewOwner(s, in) == ViewOwner::Returning);
}
```

- [ ] **Step 2: Run** `build.bat test`. Expected: compile error (missing header).

- [ ] **Step 3: Implement** `src/view_target.h`

```cpp
#pragma once
// Who owns the zoomed view (issue #276). Pure: no <windows.h>, tests/test_view_target.cpp.
// Most recent input wins; the mouse takes the view back only on REAL movement (3 px within 100 ms)
// or a button, so sensor jitter while typing never steals it. Leaving Caret/Focus always goes via
// Returning, a glide back to the pointer that the caller ends with FinishReturn().
#include <cmath>
namespace wind {
enum class ViewOwner { Mouse, Caret, Focus, Returning };
enum class TrackKind { None = 0, Caret = 1, Focus = 2 };
struct TrackSnapshot { TrackKind kind = TrackKind::None; unsigned seq = 0; double l = 0, t = 0, r = 0, b = 0; };
struct ViewOwnerState { ViewOwner owner = ViewOwner::Mouse; unsigned lastSeq = 0; double moveAccum = 0; double moveWindowMs = 0; };
struct ViewOwnerInputs {
    bool enabled = false;
    bool trackCaret = false, trackFocus = false;
    double mouseDx = 0, mouseDy = 0;
    bool buttonDown = false;
    double dtMs = 0;
    TrackSnapshot snap;
};
inline constexpr double kMouseTakeoverPx = 3.0, kMouseTakeoverWindowMs = 100.0;

inline void FinishReturn(ViewOwnerState& s) { s.owner = ViewOwner::Mouse; }

inline ViewOwner StepViewOwner(ViewOwnerState& s, const ViewOwnerInputs& in) {
    const bool detached = s.owner == ViewOwner::Caret || s.owner == ViewOwner::Focus;
    if (!in.enabled) {
        if (detached) s.owner = ViewOwner::Returning;
        s.lastSeq = in.snap.seq;     // events seen while disabled never fire later
        return s.owner;
    }
    // Mouse activity: accumulate movement inside a sliding window.
    const double step = std::fabs(in.mouseDx) + std::fabs(in.mouseDy);
    if (step > 0) {
        if (s.moveWindowMs > kMouseTakeoverWindowMs) { s.moveAccum = 0; s.moveWindowMs = 0; }
        s.moveAccum += step;
    }
    s.moveWindowMs += in.dtMs;
    if (s.moveWindowMs > kMouseTakeoverWindowMs && step == 0) { s.moveAccum = 0; s.moveWindowMs = 0; }
    const bool mouseActive = in.buttonDown || s.moveAccum >= kMouseTakeoverPx;
    if (mouseActive) {
        s.moveAccum = 0; s.moveWindowMs = 0;
        if (detached) s.owner = ViewOwner::Returning;
        s.lastSeq = in.snap.seq;     // the mouse wins this tick
        return s.owner;
    }
    // A new tracking event.
    if (in.snap.seq != s.lastSeq) {
        s.lastSeq = in.snap.seq;
        if (in.snap.kind == TrackKind::Caret && in.trackCaret) s.owner = ViewOwner::Caret;
        else if (in.snap.kind == TrackKind::Focus && in.trackFocus) s.owner = ViewOwner::Focus;
    }
    return s.owner;
}
}  // namespace wind
```

- [ ] **Step 4: Run** `build.bat test`. Expected: PASS.
- [ ] **Step 5: Commit** `feat(tracking): view owner state machine (#276)`.

### Task 3: Glide and target geometry (`view_glide.h`)

**Files:**
- Create: `src/view_glide.h`
- Test: `tests/test_view_glide.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces:

```cpp
namespace wind {
double GlideToward(double cur, double target, double dtMs, double glideMs);
struct TrackRect { double l, t, r, b; };
// Returns false (and leaves out untouched) for a rect that is empty, degenerate or off-monitor.
bool TrackTargetCenter(const TrackRect& rc, double curCx, double curCy, double level,
                       int monW, int monH, int align /*0 centred, 1 edges*/, int marginPct,
                       double& outCx, double& outCy);
}
```
Rect coordinates are monitor-local physical pixels (the caller subtracts the monitor origin).

- [ ] **Step 1: Write the failing tests** (`tests/test_view_glide.cpp`)

```cpp
#include "doctest.h"
#include "../src/view_glide.h"
using namespace wind;

TEST_CASE("glide is time-based: two 7 ms steps equal one 14 ms step") {
    double a = GlideToward(GlideToward(0, 100, 7, 150), 100, 7, 150);
    double b = GlideToward(0, 100, 14, 150);
    CHECK(a == doctest::Approx(b).epsilon(1e-9));
}
TEST_CASE("glide covers 95% of the distance in glideMs and never overshoots") {
    double v = 0; for (int i = 0; i < 150; ++i) v = GlideToward(v, 1000, 1, 150);
    CHECK(v == doctest::Approx(950).epsilon(0.01));
    CHECK(v <= 1000);
    CHECK(GlideToward(0, 1000, 0, 150) == 0);          // no time, no motion
    CHECK(GlideToward(0, 1000, 5, 0) == 1000);         // glideMs 0 means snap
}
TEST_CASE("retargeting mid-glide never moves backwards") {
    double v = 0; v = GlideToward(v, 100, 30, 150);
    double w = GlideToward(v, 110, 30, 150);
    CHECK(w > v);
}
TEST_CASE("centred: the target is the rect centre, clamped to the monitor") {
    double cx, cy;
    REQUIRE(TrackTargetCenter({1000, 500, 1002, 520}, 0, 0, 3, 3840, 2160, 0, 15, cx, cy));
    CHECK(cx == doctest::Approx(1001)); CHECK(cy == doctest::Approx(510));
}
TEST_CASE("degenerate and off-monitor rects are rejected") {
    double cx = 7, cy = 7;
    CHECK_FALSE(TrackTargetCenter({0, 0, 0, 0}, 0, 0, 3, 3840, 2160, 0, 15, cx, cy));
    CHECK_FALSE(TrackTargetCenter({500, 500, 400, 600}, 0, 0, 3, 3840, 2160, 0, 15, cx, cy));
    CHECK_FALSE(TrackTargetCenter({-900, 100, -880, 120}, 0, 0, 3, 3840, 2160, 0, 15, cx, cy));
    CHECK_FALSE(TrackTargetCenter({4000, 100, 4010, 120}, 0, 0, 3, 3840, 2160, 0, 15, cx, cy));
    CHECK(cx == 7); CHECK(cy == 7);
}
TEST_CASE("within edges: a rect already inside the margin does not move the view") {
    // level 4 on 3840x2160: view 960x540 around (1920,1080) -> x 1440..2400, y 810..1350
    double cx, cy;
    REQUIRE(TrackTargetCenter({1900, 1000, 1902, 1020}, 1920, 1080, 4, 3840, 2160, 1, 15, cx, cy));
    CHECK(cx == doctest::Approx(1920)); CHECK(cy == doctest::Approx(1080));
}
TEST_CASE("within edges: a rect past the right margin moves the view just enough") {
    // right margin edge = 2400 - 0.15*960 = 2256; rect right 2300 -> shift 44
    double cx, cy;
    REQUIRE(TrackTargetCenter({2298, 1000, 2300, 1020}, 1920, 1080, 4, 3840, 2160, 1, 15, cx, cy));
    CHECK(cx == doctest::Approx(1964)); CHECK(cy == doctest::Approx(1080));
}
TEST_CASE("oversized rect aligns top-left") {
    // a 2000 px wide rect in a 960 px view: its left edge goes to the left margin
    // (1000 - 144 + 480 = 1336, inside the monitor clamp band 480..3360)
    double cx, cy;
    REQUIRE(TrackTargetCenter({1000, 1000, 3000, 1020}, 1920, 1080, 4, 3840, 2160, 1, 15, cx, cy));
    CHECK(cx == doctest::Approx(1000 - 0.15 * 960 + 480));
}
```

- [ ] **Step 2: Run** `build.bat test`. Expected: compile error.

- [ ] **Step 3: Implement** `src/view_glide.h`

```cpp
#pragma once
// Glide and target geometry for tracking (issue #276). Pure; tests/test_view_glide.cpp.
#include <cmath>
namespace wind {

// Time-based ease: 95% of the gap in glideMs whatever the tick interval (VRR-safe, the same
// reasoning as CursorMapper::setTickDeltaMs). keep = 0.05^(dt/glideMs).
inline double GlideToward(double cur, double target, double dtMs, double glideMs) {
    if (glideMs <= 0.0) return target;
    if (dtMs <= 0.0) return cur;
    const double keep = std::pow(0.05, dtMs / glideMs);
    return target + (cur - target) * keep;
}

struct TrackRect { double l, t, r, b; };

inline bool TrackTargetCenter(const TrackRect& rc, double curCx, double curCy, double level,
                              int monW, int monH, int align, int marginPct,
                              double& outCx, double& outCy) {
    if (level < 1.0) level = 1.0;
    const double w = rc.r - rc.l, h = rc.b - rc.t;
    if (w < 0 || h < 0 || (w == 0 && h == 0)) return false;                 // empty / inverted
    if (rc.l == 0 && rc.t == 0 && rc.r == 0 && rc.b == 0) return false;      // the classic bogus caret
    if (rc.r < 0 || rc.b < 0 || rc.l > monW || rc.t > monH) return false;    // not on this monitor
    const double vw = monW / level, vh = monH / level;
    double cx = curCx, cy = curCy;
    if (align == 0) {
        cx = (rc.l + rc.r) / 2.0;
        cy = (rc.t + rc.b) / 2.0;
    } else {
        const double mx = vw * (marginPct / 100.0), my = vh * (marginPct / 100.0);
        auto axis = [](double c, double lo, double hi, double v, double m) {
            const double inLo = c - v / 2 + m, inHi = c + v / 2 - m;   // the comfortable band
            if (hi - lo > inHi - inLo) return lo - m + v / 2;          // oversized: align its start
            if (lo < inLo) return c - (inLo - lo);
            if (hi > inHi) return c + (hi - inHi);
            return c;
        };
        cx = axis(curCx, rc.l, rc.r, vw, mx);
        cy = axis(curCy, rc.t, rc.b, vh, my);
    }
    // Clamp so the view stays on the monitor (ComputeOffsetF clamps too; keeping the centre
    // consistent avoids a glide that aims past the edge and then stalls).
    const double minX = vw / 2, maxX = monW - vw / 2, minY = vh / 2, maxY = monH - vh / 2;
    outCx = cx < minX ? minX : (cx > maxX ? maxX : cx);
    outCy = cy < minY ? minY : (cy > maxY ? maxY : cy);
    return true;
}
}  // namespace wind
```

- [ ] **Step 4: Run** `build.bat test`. Expected: PASS. (Note: the centred test's expected values sit inside the clamp band at level 3.)
- [ ] **Step 5: Commit** `feat(tracking): glide and target geometry (#276)`.

### Task 4: Detached frame (`detached_view.h`)

**Files:**
- Create: `src/detached_view.h`
- Test: `tests/test_detached_view.cpp`

**Interfaces:**
- Consumes: `MapResult` (`src/cursor_mapper.h`), `ComputeOffsetF` (`src/transform.h`).
- Produces: `MapResult DetachedMap(double viewCx, double viewCy, double ptrX, double ptrY, double level, int monW, int monH);` View at (viewCx, viewCy); `cursorScreen*` and `clickDesktop*` report the real pointer (monitor-local px); `center*` is the view centre (the transform model derives its source from it).

- [ ] **Step 1: Write the failing test**

```cpp
#include "doctest.h"
#include "../src/detached_view.h"
using namespace wind;

TEST_CASE("detached map: view from the given centre, cursor fields from the real pointer") {
    MapResult r = DetachedMap(1920, 1080, 1000, 500, 4, 3840, 2160);
    CHECK(r.centerX == doctest::Approx(1920)); CHECK(r.centerY == doctest::Approx(1080));
    CHECK(r.srcLeft == doctest::Approx(1920 - 480)); CHECK(r.srcTop == doctest::Approx(1080 - 270));
    CHECK(r.clickDesktopX == 1000); CHECK(r.clickDesktopY == 500);
    CHECK(r.cursorScreenX == doctest::Approx((1000 - 1440) * 4.0));   // off to the left: not visible
    CHECK(r.cursorScreenY == doctest::Approx((500 - 810) * 4.0));
}
```

- [ ] **Step 2: Run** `build.bat test`. Expected: compile error.
- [ ] **Step 3: Implement**

```cpp
#pragma once
// A frame whose view is NOT centred on the pointer (issue #276): caret/focus tracking and, later,
// mouse edge mode. The view comes from (viewCx, viewCy); the cursor fields report the REAL pointer,
// so the sprite / drawn cursor sit where the pointer actually is and scroll out of view with the
// content. Pure; tests/test_detached_view.cpp.
#include "cursor_mapper.h"
#include "transform.h"
namespace wind {
inline MapResult DetachedMap(double viewCx, double viewCy, double ptrX, double ptrY, double level,
                             int monW, int monH) {
    if (level < 1.0) level = 1.0;
    const OffsetF o = ComputeOffsetF(viewCx, viewCy, level, monW, monH);
    MapResult r;
    r.srcLeft = o.x; r.srcTop = o.y;
    r.centerX = viewCx; r.centerY = viewCy;
    r.cursorScreenX = (ptrX - o.x) * level;
    r.cursorScreenY = (ptrY - o.y) * level;
    r.clickDesktopX = static_cast<int>(ptrX + (ptrX >= 0 ? 0.5 : -0.5));
    r.clickDesktopY = static_cast<int>(ptrY + (ptrY >= 0 ? 0.5 : -0.5));
    return r;
}
}  // namespace wind
```

- [ ] **Step 4: Run** `build.bat test`. Expected: PASS. If `ComputeOffsetF` clamps differently than assumed, adjust the expected `srcLeft/srcTop` to its documented formula in `src/transform.h`, not the other way round.
- [ ] **Step 5: Commit** `feat(tracking): detached view frame (#276)`.

### Task 5: The watcher thread (`focus_track`)

**Files:**
- Create: `src/focus_track.h`, `src/focus_track.cpp`
- Modify: `build.bat` (add `oleaut32.lib uuid.lib` to both app link lines if missing; `ole32.lib` is already there)

**Interfaces:**
- Consumes: `TrackSnapshot`, `TrackKind` (Task 2), `wind::Log`.
- Produces:

```cpp
namespace wind {
class FocusTracker {
public:
    bool start();                        // spawns the thread; idempotent
    void stop();                         // joins; unhooks; CoUninitialize on the thread
    void setActive(bool on, bool wantCaret, bool wantFocus, bool log);   // tick thread, cheap
    TrackSnapshot snapshot() const;      // tick thread, copies under a mutex
};
}
```
All rects published in physical virtual-desktop pixels (Wind is Per-Monitor-V2 aware; UIA returns physical px to a PMv2 client).

- [ ] **Step 1: Header** `src/focus_track.h`

```cpp
#pragma once
// Caret and keyboard-focus watcher (issue #276). One thread owns every accessibility call:
// WinEvents (out of context), GetGUIThreadInfo, and UI Automation on a COM MTA. The tick thread only
// flips setActive() and copies snapshot(); it never waits on UIA. See the spec, section 4.2.
#include "view_target.h"
#include <atomic>
#include <mutex>
#include <thread>
namespace wind {
class FocusTracker {
public:
    ~FocusTracker() { stop(); }
    bool start();
    void stop();
    void setActive(bool on, bool wantCaret, bool wantFocus, bool log);
    TrackSnapshot snapshot() const { std::lock_guard<std::mutex> g(mu_); return snap_; }
private:
    void run();
    friend struct FocusTrackImpl;
    std::thread th_;
    std::atomic<unsigned long> tid_{0};
    std::atomic<bool> active_{false}, wantCaret_{false}, wantFocus_{false}, log_{false};
    mutable std::mutex mu_;
    TrackSnapshot snap_;
    unsigned seq_ = 0;
    void publish(TrackKind k, double l, double t, double r, double b, const char* src);
};
}  // namespace wind
```

- [ ] **Step 2: Implementation** `src/focus_track.cpp`

```cpp
#include "focus_track.h"
#include "logging.h"
#include <windows.h>
#include <objbase.h>
#include <oleacc.h>
#include <UIAutomation.h>
#include <string>
#pragma comment(lib, "oleacc.lib")
namespace wind {

static const UINT kWakeMsg = WM_APP + 0x61;       // an event arrived: resolve after coalescing
static const UINT_PTR kCoalesceTimer = 1, kPollTimer = 2;
static FocusTracker* g_self = nullptr;            // WinEvent callbacks have no context pointer

struct FocusTrackImpl {
    static void CALLBACK OnWinEvent(HWINEVENTHOOK, DWORD ev, HWND hwnd, LONG obj, LONG child, DWORD, DWORD) {
        if (!g_self || !g_self->active_.load()) return;
        if (ev == EVENT_OBJECT_LOCATIONCHANGE && obj != OBJID_CARET) return;
        PostThreadMessageW(g_self->tid_.load(), kWakeMsg, (WPARAM)ev, 0);
    }
};

// UIA focus-changed handler: just wakes the thread (resolution happens there, coalesced).
class FocusHandler : public IUIAutomationFocusChangedEventHandler {
    LONG refs_ = 1;
public:
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs_); }
    ULONG STDMETHODCALLTYPE Release() override { LONG r = InterlockedDecrement(&refs_); if (!r) delete this; return r; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** pp) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IUIAutomationFocusChangedEventHandler)) { *pp = this; AddRef(); return S_OK; }
        *pp = nullptr; return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE HandleFocusChangedEvent(IUIAutomationElement*) override {
        if (g_self && g_self->active_.load()) PostThreadMessageW(g_self->tid_.load(), kWakeMsg, (WPARAM)EVENT_OBJECT_FOCUS, 0);
        return S_OK;
    }
};

static bool IsOwnOrTooltip(HWND h) {
    if (!h) return true;
    DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId()) return true;
    wchar_t cls[64] = {}; GetClassNameW(h, cls, 64);
    return wcscmp(cls, L"tooltips_class32") == 0 || wcscmp(cls, L"Xaml_WindowedPopupClass") == 0;
}

// Classic Win32 caret of the foreground thread, in screen px. False when there is none.
static bool Win32Caret(RECT& out) {
    HWND fg = GetForegroundWindow();
    if (IsOwnOrTooltip(fg)) return false;
    GUITHREADINFO gi{ sizeof(gi) };
    if (!GetGUIThreadInfo(GetWindowThreadProcessId(fg, nullptr), &gi) || !gi.hwndCaret) return false;
    RECT rc = gi.rcCaret;
    if (rc.right <= rc.left && rc.bottom <= rc.top) return false;
    POINT a{ rc.left, rc.top }, b{ rc.right, rc.bottom };
    if (!ClientToScreen(gi.hwndCaret, &a) || !ClientToScreen(gi.hwndCaret, &b)) return false;
    out = { a.x, a.y, b.x, b.y };
    return true;
}

static bool RangeRect(IUIAutomationTextRange* range, RECT& out) {
    SAFEARRAY* sa = nullptr;
    if (FAILED(range->GetBoundingRectangles(&sa)) || !sa) return false;
    bool ok = false;
    double* d = nullptr;
    LONG n = sa->rgsabound[0].cElements;
    if (n >= 4 && SUCCEEDED(SafeArrayAccessData(sa, (void**)&d))) {
        out = { (LONG)d[0], (LONG)d[1], (LONG)(d[0] + (d[2] > 1 ? d[2] : 1)), (LONG)(d[1] + d[3]) };
        ok = d[3] > 0;
        SafeArrayUnaccessData(sa);
    } else if (n == 0) {
        // An empty caret range has no rectangle: widen it by one character, then use its left edge.
        IUIAutomationTextRange* wide = nullptr;
        if (SUCCEEDED(range->Clone(&wide)) && wide) {
            int moved = 0;
            if (SUCCEEDED(wide->ExpandToEnclosingUnit(TextUnit_Character)) &&
                SUCCEEDED(wide->GetBoundingRectangles(&sa)) && sa && sa->rgsabound[0].cElements >= 4 &&
                SUCCEEDED(SafeArrayAccessData(sa, (void**)&d))) {
                out = { (LONG)d[0], (LONG)d[1], (LONG)d[0] + 2, (LONG)(d[1] + d[3]) };
                ok = d[3] > 0;
                SafeArrayUnaccessData(sa);
            }
            (void)moved;
            wide->Release();
        }
    }
    if (sa) SafeArrayDestroy(sa);
    return ok;
}

bool FocusTracker::start() {
    if (th_.joinable()) return true;
    g_self = this;
    th_ = std::thread([this] { run(); });
    return true;
}
void FocusTracker::stop() {
    const unsigned long t = tid_.load();
    if (t) PostThreadMessageW(t, WM_QUIT, 0, 0);
    if (th_.joinable()) th_.join();
    if (g_self == this) g_self = nullptr;
}
void FocusTracker::setActive(bool on, bool wantCaret, bool wantFocus, bool log) {
    wantCaret_ = wantCaret; wantFocus_ = wantFocus; log_ = log;
    const bool was = active_.exchange(on);
    if (on && !was) { const unsigned long t = tid_.load(); if (t) PostThreadMessageW(t, kWakeMsg, 0, 0); }
}
void FocusTracker::publish(TrackKind k, double l, double t, double r, double b, const char* src) {
    {
        std::lock_guard<std::mutex> g(mu_);
        if (snap_.kind == k && snap_.l == l && snap_.t == t && snap_.r == r && snap_.b == b) return;  // unchanged
        snap_ = { k, ++seq_, l, t, r, b };
    }
    if (log_) wind::Log(wind::LogLevel::Info, "track", "%s via %s: %.0f,%.0f %.0fx%.0f",
                        k == TrackKind::Caret ? "caret" : "focus", src, l, t, r - l, b - t);
}

void FocusTracker::run() {
    tid_ = GetCurrentThreadId();
    MSG m; PeekMessageW(&m, nullptr, WM_USER, WM_USER, PM_NOREMOVE);   // make the queue exist
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IUIAutomation* uia = nullptr;
    CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, __uuidof(IUIAutomation), (void**)&uia);
    FocusHandler* fh = nullptr;
    if (uia) { fh = new FocusHandler(); if (FAILED(uia->AddFocusChangedEventHandler(nullptr, fh))) { fh->Release(); fh = nullptr; } }
    HWINEVENTHOOK h1 = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, FocusTrackImpl::OnWinEvent, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    HWINEVENTHOOK h2 = SetWinEventHook(EVENT_SYSTEM_MENUPOPUPSTART, EVENT_SYSTEM_MENUPOPUPSTART, nullptr, FocusTrackImpl::OnWinEvent, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    HWINEVENTHOOK h3 = SetWinEventHook(EVENT_OBJECT_FOCUS, EVENT_OBJECT_FOCUS, nullptr, FocusTrackImpl::OnWinEvent, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    HWINEVENTHOOK h4 = SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE, nullptr, FocusTrackImpl::OnWinEvent, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    SetTimer(nullptr, kPollTimer, 16, nullptr);   // 60 Hz backstop, work only while active
    bool pendingFocus = false, pendingCaret = false;
    UINT_PTR coalesce = 0;

    auto resolve = [&](bool focusChanged) {
        if (!active_.load()) return;
        HWND fg = GetForegroundWindow();
        if (IsOwnOrTooltip(fg)) return;
        RECT rc{};
        // 1. Caret, fastest source first.
        if (wantCaret_.load()) {
            if (Win32Caret(rc)) { publish(TrackKind::Caret, rc.left, rc.top, rc.right, rc.bottom, "win32"); return; }
            IUIAutomationElement* el = nullptr;
            if (uia && SUCCEEDED(uia->GetFocusedElement(&el)) && el) {
                IUIAutomationTextPattern2* tp2 = nullptr;
                if (SUCCEEDED(el->GetCurrentPatternAs(UIA_TextPattern2Id, __uuidof(IUIAutomationTextPattern2), (void**)&tp2)) && tp2) {
                    BOOL active = FALSE; IUIAutomationTextRange* cr = nullptr;
                    if (SUCCEEDED(tp2->GetCaretRange(&active, &cr)) && cr) {
                        if (active && RangeRect(cr, rc)) { cr->Release(); tp2->Release(); el->Release();
                            publish(TrackKind::Caret, rc.left, rc.top, rc.right, rc.bottom, "uia-caret"); return; }
                        cr->Release();
                    }
                    tp2->Release();
                }
                IUIAutomationTextPattern* tp = nullptr;
                if (SUCCEEDED(el->GetCurrentPatternAs(UIA_TextPatternId, __uuidof(IUIAutomationTextPattern), (void**)&tp)) && tp) {
                    IUIAutomationTextRangeArray* sel = nullptr;
                    if (SUCCEEDED(tp->GetSelection(&sel)) && sel) {
                        int n = 0; sel->get_Length(&n);
                        IUIAutomationTextRange* r0 = nullptr;
                        if (n > 0 && SUCCEEDED(sel->GetElement(0, &r0)) && r0) {
                            bool ok = RangeRect(r0, rc); r0->Release();
                            if (ok) { sel->Release(); tp->Release(); el->Release();
                                publish(TrackKind::Caret, rc.left, rc.top, rc.right, rc.bottom, "uia-selection"); return; }
                        }
                        sel->Release();
                    }
                    tp->Release();
                }
                // 2. Focus: the element's own bounds, only on a real focus change.
                if (focusChanged && wantFocus_.load()) {
                    RECT b{};
                    if (SUCCEEDED(el->get_CurrentBoundingRectangle(&b)) && b.right > b.left && b.bottom > b.top) {
                        el->Release(); publish(TrackKind::Focus, b.left, b.top, b.right, b.bottom, "uia-focus"); return;
                    }
                }
                el->Release();
            }
        } else if (focusChanged && wantFocus_.load() && uia) {
            IUIAutomationElement* el = nullptr; RECT b{};
            if (SUCCEEDED(uia->GetFocusedElement(&el)) && el) {
                if (SUCCEEDED(el->get_CurrentBoundingRectangle(&b)) && b.right > b.left && b.bottom > b.top)
                    publish(TrackKind::Focus, b.left, b.top, b.right, b.bottom, "uia-focus");
                el->Release();
            }
        }
    };

    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (m.message == kWakeMsg) {
            if (m.wParam == EVENT_OBJECT_FOCUS || m.wParam == EVENT_SYSTEM_FOREGROUND || m.wParam == EVENT_SYSTEM_MENUPOPUPSTART) pendingFocus = true;
            else pendingCaret = true;
            if (!coalesce) coalesce = SetTimer(nullptr, kCoalesceTimer, 30, nullptr);   // NVDA's ~30 ms
        } else if (m.message == WM_TIMER && m.wParam == coalesce && coalesce) {
            KillTimer(nullptr, coalesce); coalesce = 0;
            resolve(pendingFocus); pendingFocus = pendingCaret = false;
        } else if (m.message == WM_TIMER) {
            if (active_.load() && wantCaret_.load()) resolve(false);                    // backstop poll
        }
        TranslateMessage(&m); DispatchMessageW(&m);
    }
    KillTimer(nullptr, kPollTimer);
    for (HWINEVENTHOOK h : { h1, h2, h3, h4 }) if (h) UnhookWinEvent(h);
    if (uia && fh) uia->RemoveFocusChangedEventHandler(fh);
    if (fh) fh->Release();
    if (uia) uia->Release();
    CoUninitialize();
    tid_ = 0;
}
}  // namespace wind
```

Notes for the implementer:
- `SetTimer(nullptr, id, ...)` ignores `id` and returns a new one; compare `m.wParam` against the returned value (as above for `coalesce`); the poll timer is the other `WM_TIMER`.
- The poll only runs work while `active_` is true, so an idle Wind does nothing.
- Filtering the caret to the foreground window is done by querying the foreground thread and the focused element only; stale carets of background windows never enter.

- [ ] **Step 3: Build** `build.bat` and `build.bat check`. Expected: no errors. Add `oleacc.lib` handled by the pragma; add `oleaut32.lib uuid.lib` to the link lines in `build.bat` (both the normal and `uiaccess` targets) if the linker reports `SafeArray*` or `IID_*` unresolved.
- [ ] **Step 4: Commit** `feat(tracking): caret and focus watcher thread (#276)`.

### Task 6: RunTick integration

**Files:**
- Modify: `src/main.cpp` (tick state struct near `restOverlapTicks` at ~line 284; the regime seam before `t.mapper.update` at ~line 1452; `ex.suppressCursorSync` at ~line 1673; startup/shutdown where `g_input` starts/stops)

**Interfaces:**
- Consumes: `FocusTracker` (Task 5), `StepViewOwner`/`FinishReturn` (Task 2), `GlideToward`/`TrackTargetCenter` (Task 3), `DetachedMap` (Task 4), Config keys (Task 1).

- [ ] **Step 1: State and lifetime.** Add to the tick state struct:

```cpp
    // Tracking (issue #276): who owns the view, and the glided detached centre (monitor-local px).
    wind::ViewOwnerState viewOwner;
    double viewCx = 0.0, viewCy = 0.0;
    bool   viewDetached = false;   // last tick drew a detached frame
```

Add a file-scope `static wind::FocusTracker g_track;`, call `g_track.start()` next to where the input router starts, and `g_track.stop()` on every shutdown path where the input router is stopped.

- [ ] **Step 2: Step the owner and build the detached frame.** Immediately after `MapResult r = t.mapper.update(freeCursor ? 0 : dx, freeCursor ? 0 : dy, lvl);`:

```cpp
        // --- Tracking (issue #276): caret / focus own the view; the pointer is never moved. ---
        const bool trackEnabled = lvl > 1.001 && !inspect && !t.detector.locked() && !fsGameSession &&
                                  (t.cfg.trackCaret != 0 || t.cfg.trackFocus != 0);
        g_track.setActive(trackEnabled, t.cfg.trackCaret != 0, t.cfg.trackFocus != 0, t.cfg.trackLog != 0);
        {
            wind::ViewOwnerInputs vi;
            vi.enabled = trackEnabled;
            vi.trackCaret = t.cfg.trackCaret != 0; vi.trackFocus = t.cfg.trackFocus != 0;
            vi.mouseDx = curDx; vi.mouseDy = curDy;
            vi.buttonDown = (GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_RBUTTON) | GetAsyncKeyState(VK_MBUTTON)) & 0x8000;
            vi.dtMs = dt * 1000.0;
            vi.snap = g_track.snapshot();
            const bool wasDetached = t.viewOwner.owner != wind::ViewOwner::Mouse;
            const wind::ViewOwner owner = wind::StepViewOwner(t.viewOwner, vi);
            if (owner != wind::ViewOwner::Mouse) {
                if (!wasDetached) { t.viewCx = r.centerX; t.viewCy = r.centerY; }   // glide from where we are
                const double ptrX = cur.x - t.mon.x, ptrY = cur.y - t.mon.y;
                double tx = t.viewCx, ty = t.viewCy;
                if (owner == wind::ViewOwner::Returning) {
                    tx = ptrX; ty = ptrY;   // centred return; phase 2 swaps in EdgePanCenter for mouseAlign=1
                } else {
                    const wind::TrackRect rc{ vi.snap.l - t.mon.x, vi.snap.t - t.mon.y, vi.snap.r - t.mon.x, vi.snap.b - t.mon.y };
                    double ox, oy;
                    if (wind::TrackTargetCenter(rc, t.viewCx, t.viewCy, lvl, t.mon.w, t.mon.h,
                                                t.cfg.trackAlign, t.cfg.trackMarginPct, ox, oy)) { tx = ox; ty = oy; }
                }
                t.viewCx = wind::GlideToward(t.viewCx, tx, vi.dtMs, t.cfg.trackGlideMs);
                t.viewCy = wind::GlideToward(t.viewCy, ty, vi.dtMs, t.cfg.trackGlideMs);
                if (owner == wind::ViewOwner::Returning &&
                    std::fabs(t.viewCx - tx) < 1.0 && std::fabs(t.viewCy - ty) < 1.0) {
                    wind::FinishReturn(t.viewOwner);
                    t.mapper.reset(ptrX, ptrY);          // hand back exactly at the pointer
                    t.lastSetVirtual = cur;
                } else {
                    r = wind::DetachedMap(t.viewCx, t.viewCy, ptrX, ptrY, lvl, t.mon.w, t.mon.h);
                    t.mapper.reset(t.viewCx, t.viewCy);   // hybrid switches and the next tick start here
                    t.lastSetVirtual = cur;               // measure the next hand motion from here
                    t.viewDetached = true;
                }
            } else {
                t.viewDetached = false;
            }
        }
```

`fsGameSession` means: use the existing variable the tick already computes for "foreground covers the monitor / game" (search `fsGame` near `ex.fsGame`); if it is computed later in the tick, hoist that computation above this block rather than re-deriving it.

- [ ] **Step 3: Never weld while detached.** Change `ex.suppressCursorSync = dragFollow || freeCursor;` to:

```cpp
        ex.suppressCursorSync = dragFollow || freeCursor || t.viewDetached;   // tracking never moves the pointer (#276)
```

- [ ] **Step 4: Disarm the hook write path while detached.** The input hook can write the transform from pointer events (`txHookWrite`, the `hookWrite` condition near `wind::PublishHookTransform`); it would drag the view back to the pointer. Add `&& !t.viewDetached` to that condition:

```cpp
        const bool hookWrite = freeCursor && t.cfg.txHookWrite != 0 && wind::MagThreadOwned() &&
                               tmWall != nullptr && lvl > 1.0 && levelSettled && !t.viewDetached;
```

If that condition is evaluated before the tracking block in the tick, use `t.viewOwner.owner != wind::ViewOwner::Mouse` (the state from the previous tick) instead of `t.viewDetached`.

- [ ] **Step 5: Build and unit tests** `build.bat test` then `build.bat` and `build.bat check`. Expected: all pass, no errors.
- [ ] **Step 6: Commit** `feat(tracking): drive the view from caret and focus in RunTick (#276)`.

### Task 7: Settings rows

**Files:**
- Modify: `ui/src/settings-schema.js` (new "Tracking" section after the cursor section)
- Modify: `ui/src/lib/icons.js` if a section icon is required (reuse an existing icon if the schema allows)
- Test: `ui/tests/settings.spec.js`

- [ ] **Step 1: Write the failing Playwright test** (append)

```js
test('Tracking section: caret on, focus off, centred by default (issue #276)', async ({ page }) => {
  await page.goto('/');
  const caret = page.getByRole('switch', { name: 'Follow the text cursor' });
  const focus = page.getByRole('switch', { name: 'Follow keyboard focus' });
  await expect(caret).toBeChecked();
  await expect(focus).not.toBeChecked();
  await focus.click();
  await page.getByRole('button', { name: 'Apply' }).click();
  const sets = await page.evaluate(() => window.__sets.filter(m => m.type === 'setConfig' && m.key === 'trackFocus'));
  expect(sets.at(-1).value).toBe('1');
});
```

Match the role/name query to how existing toggle rows are exposed in `Row.svelte` (the existing tests show the pattern); if toggles are exposed as `checkbox`, use that role.

- [ ] **Step 2: Run** `npx playwright test` in `ui/`. Expected: FAIL (rows missing).
- [ ] **Step 3: Add the section** to `settings-schema.js`:

```js
  { id:'tracking', label:'Tracking', icon:'cursor', desc:'What the zoomed view follows besides the mouse.', rows: [
    { key:'trackCaret', type:'toggle', label:'Follow the text cursor', desc:'While you type, the view glides to the text cursor. The mouse pointer stays where it was.', def:'1' },
    { key:'trackFocus', type:'toggle', label:'Follow keyboard focus', desc:'When you move with Tab or the arrow keys, the view glides to the selected control.', def:'0' },
    { key:'trackAlign', type:'select', label:'Keep the text cursor and focus', options:['0','1'], optionLabels:{ '0':'Centred', '1':'Within the edges' }, def:'0' },
  ]},
```

Use the exact field names the other rows in the file use (`type`, `options`, `optionLabels`, `def`); copy the shape of an existing toggle and select row rather than trusting this snippet if they differ.

- [ ] **Step 4: Run** `npx playwright test`. Expected: all pass.
- [ ] **Step 5: Commit** `feat(settings): Tracking section (#276)`.

### Task 8: Field verification, docs, PR A

- [ ] **Step 1:** Bump `src/version.h` to `0.10.3`.
- [ ] **Step 2:** Deploy the signed build (`tools/uiaccess_setup.ps1`, elevated, absolute path), set `trackLog=1`, zoom 3x.
- [ ] **Step 3: Field matrix** (owner at the PC; record each result and the logged source): Notepad (expect `win32`), Word, Chrome text box and Google Docs (expect `uia-caret`), VS Code editor, Prism and Windows Terminal (expect `uia-selection`), Settings with Tab and `trackFocus=1` (expect `uia-focus`), a right-click menu, the Start menu search box. For each: the view glides to the caret, moving the mouse returns the view to the pointer, typing with a hand resting on the mouse does not steal the view.
- [ ] **Step 4: No-regression checks:** zoom over DOOM (game: tracking inactive, unchanged), Inspect toggle, drag a window while zoomed, the Snipping Tool.
- [ ] **Step 5: Docs:** add a "Tracking modes" block to `CLAUDE.md` (owner model, weld veto, watcher thread, sources and `trackLog`), a section in `docs/architecture/07-cursor.md`, and record the field matrix results (which source per app, any gaps) in `docs/TRACKING-FINDINGS.md`.
- [ ] **Step 6:** `tools/release.ps1` elevated through pwsh 7 (installer checks), then open PR A and ask the owner "merge?".

## Phase 2 (PR B): mouse edge mode

### Task 9: Edge pan geometry (`edge_pan.h`)

**Files:**
- Create: `src/edge_pan.h`
- Test: `tests/test_edge_pan.cpp`

**Interfaces:**
- Produces: `void EdgePanCenter(double curCx, double curCy, double ptrX, double ptrY, double level, int monW, int monH, int marginPct, double maxSrcX, double maxSrcY, double& outCx, double& outCy);` Keeps the pointer inside the margin band of the view; applies the MPO walls (`maxSrcX/Y < 0` = none) to the source rect directly.

- [ ] **Step 1: Write the failing tests**

```cpp
#include "doctest.h"
#include "../src/edge_pan.h"
using namespace wind;

TEST_CASE("edge pan: a pointer inside the band leaves the view alone") {
    double cx, cy;   // level 4, 3840x2160: view 960x540 at (1920,1080), band x 1584..2256
    EdgePanCenter(1920, 1080, 2000, 1100, 4, 3840, 2160, 15, -1, -1, cx, cy);
    CHECK(cx == doctest::Approx(1920)); CHECK(cy == doctest::Approx(1080));
}
TEST_CASE("edge pan: past the right band edge the view follows just enough") {
    double cx, cy;
    EdgePanCenter(1920, 1080, 2300, 1080, 4, 3840, 2160, 15, -1, -1, cx, cy);
    CHECK(cx == doctest::Approx(1920 + 44)); CHECK(cy == doctest::Approx(1080));
}
TEST_CASE("edge pan: the view never leaves the monitor") {
    double cx, cy;
    EdgePanCenter(480, 270, 0, 0, 4, 3840, 2160, 15, -1, -1, cx, cy);
    CHECK(cx == doctest::Approx(480)); CHECK(cy == doctest::Approx(270));
}
TEST_CASE("edge pan: the MPO wall bounds the source left edge directly") {
    double cx, cy;   // wall srcLeft <= 2000 -> centre <= 2480
    EdgePanCenter(2400, 1080, 3800, 1080, 4, 3840, 2160, 15, 2000, -1, cx, cy);
    CHECK(cx == doctest::Approx(2480));
}
```

- [ ] **Step 2: Run** `build.bat test`. Expected: compile error.
- [ ] **Step 3: Implement**

```cpp
#pragma once
// Mouse edge mode (issue #276, phase 2): the view moves only when the pointer leaves the comfortable
// band (the view minus a margin on each side), and then just far enough. Pure; tests/test_edge_pan.cpp.
namespace wind {
inline void EdgePanCenter(double curCx, double curCy, double ptrX, double ptrY, double level,
                          int monW, int monH, int marginPct, double maxSrcX, double maxSrcY,
                          double& outCx, double& outCy) {
    if (level < 1.0) level = 1.0;
    const double vw = monW / level, vh = monH / level;
    const double mx = vw * marginPct / 100.0, my = vh * marginPct / 100.0;
    auto axis = [](double c, double p, double v, double m) {
        const double lo = c - v / 2 + m, hi = c + v / 2 - m;
        if (p < lo) return c - (lo - p);
        if (p > hi) return c + (p - hi);
        return c;
    };
    double cx = axis(curCx, ptrX, vw, mx), cy = axis(curCy, ptrY, vh, my);
    if (maxSrcX >= 0 && cx - vw / 2 > maxSrcX) cx = maxSrcX + vw / 2;
    if (maxSrcY >= 0 && cy - vh / 2 > maxSrcY) cy = maxSrcY + vh / 2;
    const double minX = vw / 2, maxX = monW - vw / 2, minY = vh / 2, maxY = monH - vh / 2;
    outCx = cx < minX ? minX : (cx > maxX ? maxX : cx);
    outCy = cy < minY ? minY : (cy > maxY ? maxY : cy);
}
}  // namespace wind
```

- [ ] **Step 4: Run** `build.bat test`. Expected: PASS.
- [ ] **Step 5: Commit** `feat(tracking): edge pan geometry (#276)`.

### Task 10: Edge mode in RunTick, Settings, PR B

**Files:**
- Modify: `src/main.cpp` (the tracking block from Task 6, the free-cursor reset, `ex.suppressCursorSync`)
- Modify: `ui/src/settings-schema.js`, `ui/tests/settings.spec.js`

- [ ] **Step 1: Edge mode for the mouse.** In the tracking block, when the owner is `Mouse`, `t.cfg.mouseAlign == 1`, and the session is free (`!inspect && !t.detector.locked()`):

```cpp
            if (owner == wind::ViewOwner::Mouse && t.cfg.mouseAlign == 1 && !inspect && !t.detector.locked()) {
                if (!t.viewDetached) { t.viewCx = r.centerX; t.viewCy = r.centerY; }
                const double ptrX = cur.x - t.mon.x, ptrY = cur.y - t.mon.y;
                double ecx, ecy;
                wind::EdgePanCenter(t.viewCx, t.viewCy, ptrX, ptrY, lvl, t.mon.w, t.mon.h,
                                    t.cfg.trackMarginPct, wallX, wallY, ecx, ecy);
                t.viewCx = ecx; t.viewCy = ecy;   // no glide: the view is pushed by the hand directly
                r = wind::DetachedMap(t.viewCx, t.viewCy, ptrX, ptrY, lvl, t.mon.w, t.mon.h);
                t.mapper.reset(t.viewCx, t.viewCy);
                t.lastSetVirtual = cur;
                t.viewDetached = true;
            }
```

`wallX/wallY` are the values the tick already passes to `mapper.setMaxSourceLeft/Top` (reuse them; `-1` when no wall). In the `Returning` branch, when `mouseAlign == 1`, compute the return target with `EdgePanCenter` instead of centring on the pointer.

- [ ] **Step 2: Settings row** in the Tracking section:

```js
    { key:'mouseAlign', type:'select', label:'Keep the mouse pointer', options:['0','1'], optionLabels:{ '0':'Centred', '1':'Within the edges' }, def:'0' },
```

plus a Playwright test that the select defaults to "Centred" and writes `mouseAlign=1` on Apply.

- [ ] **Step 3: Tests and build:** `build.bat test`, `build.bat`, `build.bat check`, `npx playwright test`.
- [ ] **Step 4: Field check with the owner:** edges mode on the desktop (pointer roams, view pans at 15% margin, clicks land where the pointer is), both engines (render via `model=hybrid` + `desktopTransform=0`, transform via the default), a mouselook game stays centred, caret tracking still returns to the pointer the edges way.
- [ ] **Step 5:** Bump to `0.10.4`, update CLAUDE.md / `docs/architecture/07-cursor.md`, `tools/release.ps1` elevated, open PR B, ask "merge?".
