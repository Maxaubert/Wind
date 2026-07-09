# Transform Magnifier Model Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a second, selectable magnification model to Wind, a near-zero-GPU DWM fullscreen-transform magnifier ported from Bloom, chosen from WindConfig; the existing capture-and-render overlay stays the default.

**Architecture:** A small `IMagnifierModel` interface abstracts the per-tick lifecycle + draw that `RunTick` currently calls on `RenderEngine` directly. `RenderModel` wraps today's `RenderEngine` (behavior identical). A new `TransformModel` implements the interface with `MagSetFullscreenTransform` / the private `SetMagnificationDesktopMagnification` channel, a scene-locked cursor sprite (hides the OS cursor and draws its own, welded to the transform so it can never diverge near screen edges), and a 1px composition pin for flip-game pan smoothness. `wWinMain` constructs the model named by `Config::model`.

**Tech Stack:** C++17, MSVC (`build.bat`), Win32 + Magnification API (`Magnification.lib`, already linked), doctest (pure-logic unit tests), Svelte + WebView2 (WindConfig).

## Global Constraints

- **No em-dashes (U+2014) anywhere** (code, comments, docs, ini, UI copy). Use commas, en-dashes, or rephrase.
- **Pure-logic files must not include `<windows.h>`** (they compile into `wind_tests.exe` via `build.bat test`, which is desktop-free). New pure math goes in `src/transform.{h,cpp}`. All Win32 / Magnification / GDI calls live in Win32-only files that are NOT added to the `:test` line of `build.bat`.
- **The app build compiles `src\*.cpp` by wildcard**, so new `.cpp` files under `src/` are picked up automatically for `Wind.exe`. New pure files added to the test target must be appended to the `:test` `cl` line in `build.bat`.
- **`render` stays the default at every commit.** The branch must build and run with the render model behaving exactly as before after every task.
- **Branch:** `feat/transform-model`. **Issue:** #126. Commit trailer on every commit: `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`.
- **Build commands:** `build.bat` (app), `build.bat test` (pure unit tests, runs `wind_tests.exe`), `build.bat check` (compile-only, all `src\*.cpp`, no link), `build.bat config` (WindConfig + Svelte UI).

---

### Task 1: Pure Mag-transform math

Compute the Magnification API offsets from Wind's already-clamped sub-pixel source rect. The private channel wants a screen-space translation (`-source * level`) which is `level`-times finer than the whole-pixel public offset; the public fallback wants the rounded integer source top-left. Both derive from the same `srcLeft/srcTop` the `CursorMapper` already produces (via `ComputeOffsetF`).

**Files:**
- Modify: `src/transform.h` (add struct + function declaration)
- Modify: `src/transform.cpp` (add function definition)
- Create: `tests/test_transform_model.cpp`

**Interfaces:**
- Produces: `struct wind::MagTransform { int offX; int offY; int txX; int txY; };` and `MagTransform wind::ComputeMagTransform(double srcLeft, double srcTop, double level);` where `offX/offY = round(srcLeft/srcTop)` (public `MagSetFullscreenTransform` offset) and `txX/txY = round(-srcLeft*level / -srcTop*level)` (private `SetMagnificationDesktopMagnification` translation). Consumed by Task 9 (`TransformModel`).

- [ ] **Step 1: Write the failing test**

Create `tests/test_transform_model.cpp`:

```cpp
#include "doctest.h"
#include "transform.h"

using namespace wind;

TEST_CASE("ComputeMagTransform: public offset rounds the source top-left") {
    MagTransform m = ComputeMagTransform(100.4, 200.6, 2.0);
    CHECK(m.offX == 100);
    CHECK(m.offY == 201);
}

TEST_CASE("ComputeMagTransform: private translation is -source*level, level-finer") {
    // At level 3, a 0.5px source move shifts the translation by ~1.5px (rounds to 2/1),
    // where the whole-pixel offset would not move at all.
    MagTransform a = ComputeMagTransform(10.0, 10.0, 3.0);
    MagTransform b = ComputeMagTransform(10.5, 10.0, 3.0);
    CHECK(a.txX == -30);
    CHECK(b.txX == -32);              // -10.5*3 = -31.5 -> round to -32
    CHECK(a.offX == b.offX);          // public offset (round(10.0)==round(10.5)==10) does not move
}

TEST_CASE("ComputeMagTransform: zero source is identity") {
    MagTransform m = ComputeMagTransform(0.0, 0.0, 4.0);
    CHECK(m.offX == 0);
    CHECK(m.offY == 0);
    CHECK(m.txX == 0);
    CHECK(m.txY == 0);
}
```

- [ ] **Step 2: Add the test file to the test build, run, verify it fails**

In `build.bat`, on the `:test` `cl` line, the tests are picked up by `tests\*.cpp` already (no change needed for the test file). But `transform.cpp` is already listed. Run:

Run: `build.bat test`
Expected: FAIL to compile with "`MagTransform`: undeclared identifier" / "`ComputeMagTransform`: identifier not found".

- [ ] **Step 3: Declare in `transform.h`**

Append inside `namespace wind { ... }` in `src/transform.h`, after `ComputeOffsetF`:

```cpp
// The two forms of the fullscreen-magnifier transform, both derived from the sub-pixel source
// top-left (srcLeft/srcTop, screen px) the CursorMapper already clamps. off* is the whole-pixel
// offset the public MagSetFullscreenTransform takes; tx* is the screen-space translation
// (-source * level) the private SetMagnificationDesktopMagnification channel takes, which pans
// level-times more finely so slow sub-pixel drift still moves ~1px/frame instead of stalling.
struct MagTransform { int offX; int offY; int txX; int txY; };
MagTransform ComputeMagTransform(double srcLeft, double srcTop, double level);
```

- [ ] **Step 4: Define in `transform.cpp`**

Append inside `namespace wind { ... }` in `src/transform.cpp`:

```cpp
static int iround(double v) { return (int)(v < 0 ? v - 0.5 : v + 0.5); }

MagTransform ComputeMagTransform(double srcLeft, double srcTop, double level) {
    if (level < 1.0) level = 1.0;
    return MagTransform{
        iround(srcLeft), iround(srcTop),
        iround(-srcLeft * level), iround(-srcTop * level),
    };
}
```

- [ ] **Step 5: Run tests, verify pass**

Run: `build.bat test`
Expected: PASS (all `test_transform_model.cpp` cases green, existing tests still green).

- [ ] **Step 6: Commit**

```bash
git add src/transform.h src/transform.cpp tests/test_transform_model.cpp
git commit -m "feat(transform): pure Mag-transform math for the transform model (#126)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 2: Config surface (`model` + transform-only keys)

Add the model selector and the three transform-only config keys, with parsing, an unknown-value fallback to `render`, and the commented ini template block.

**Files:**
- Modify: `src/config.h` (add fields)
- Modify: `src/config.cpp` (parse + sanitize + ini template)
- Create: `tests/test_config_model.cpp`

**Interfaces:**
- Produces: `Config::model` (`std::string`, default `"render"`, values `render`|`transform`), `Config::fastPan` (`int`, default 1), `Config::smoothPan` (`int`, default 0), `Config::cursorSprite` (`int`, default 1). Consumed by Tasks 4, 9, 10.

- [ ] **Step 1: Write the failing test**

Create `tests/test_config_model.cpp`:

```cpp
#include "doctest.h"
#include "config.h"

using namespace wind;

TEST_CASE("model defaults to render") {
    Config c = ParseConfig("");
    CHECK(c.model == "render");
    CHECK(c.fastPan == 1);
    CHECK(c.smoothPan == 0);
    CHECK(c.cursorSprite == 1);
}

TEST_CASE("model=transform parses and transform keys read") {
    Config c = ParseConfig("model=transform\nfastPan=0\nsmoothPan=1\ncursorSprite=0\n");
    CHECK(c.model == "transform");
    CHECK(c.fastPan == 0);
    CHECK(c.smoothPan == 1);
    CHECK(c.cursorSprite == 0);
}

TEST_CASE("unknown model value falls back to render") {
    Config c = ParseConfig("model=bogus\n");
    CHECK(c.model == "render");
}
```

- [ ] **Step 2: Run, verify it fails**

Run: `build.bat test`
Expected: FAIL to compile with "`model` is not a member of `wind::Config`".

- [ ] **Step 3: Add fields to `src/config.h`**

Add to the `Config` struct (place near the top of the "Own GPU renderer" section, before `cursorSensitivity`, so render- and transform-model knobs are visually grouped):

```cpp
    // --- Model selection ----------------------------------------------------
    // Which magnification model runs. "render" (default) = the DXGI capture + D3D11 overlay.
    // "transform" = the low-GPU DWM fullscreen-transform model (MagSetFullscreenTransform). An
    // unknown value falls back to "render". Applied at launch (restart to switch; not hot-swapped).
    std::string model = "render";
    // Transform-model-only knobs (ignored by the render model):
    int fastPan     = 1;  // 1 = pan via the private SetMagnificationDesktopMagnification channel
                          //     (sub-pixel); falls back to the public API automatically if unavailable.
    int smoothPan   = 0;  // 1 = hold the display composited while zoomed (1px pin) so flip-model games
                          //     do not stutter while panning, at a capped frame rate while zoomed.
    int cursorSprite = 1; // 1 = hide the OS cursor and draw a scene-locked sprite welded to the
                          //     transform (fixes cursor/click divergence near screen edges).
```

- [ ] **Step 4: Parse + sanitize in `src/config.cpp`**

In the key-parsing `if/else` chain (after the `cursorVisibility` line, `src/config.cpp:105`), add:

```cpp
            else if (key == "model")              c.model = val;
            else if (key == "fastPan")            c.fastPan = std::stoi(val);
            else if (key == "smoothPan")          c.smoothPan = std::stoi(val);
            else if (key == "cursorSprite")       c.cursorSprite = std::stoi(val);
```

Then, in the clamp/sanitize block (after the numeric clamps near `src/config.cpp:139`), add the unknown-value fallback:

```cpp
    if (c.model != "render" && c.model != "transform") c.model = "render";
```

- [ ] **Step 5: Add the ini template block in `src/config.cpp`**

In the default-ini string (the concatenated `"key=val\n"` block; add near the renderer knobs, e.g. after the `hdrTonemap=1` line around `src/config.cpp:244`):

```cpp
               "; model: render = GPU capture+overlay (default, high fidelity). transform = low-GPU\n"
               ";   DWM fullscreen-transform. Restart to switch. transform ignores the render-only\n"
               ";   knobs below (sharpness, hdrTonemap, bilinear, outline, zorderBand) and cannot\n"
               ";   cover the Start menu / taskbar.\n"
               "model=render\n"
               "; fastPan (transform only): 1 = private sub-pixel pan channel; auto-falls back if absent\n"
               "fastPan=1\n"
               "; smoothPan (transform only): 1 = keep the display composited while zoomed so flip-model\n"
               ";   games do not stutter while panning (caps the frame rate while zoomed); 0 = off\n"
               "smoothPan=0\n"
               "; cursorSprite (transform only): 1 = scene-locked cursor sprite (recommended); 0 = OS cursor\n"
               "cursorSprite=1\n"
```

- [ ] **Step 6: Run tests, verify pass**

Run: `build.bat test`
Expected: PASS (all `test_config_model.cpp` cases green).

- [ ] **Step 7: Commit**

```bash
git add src/config.h src/config.cpp tests/test_config_model.cpp
git commit -m "feat(config): add model selector + transform-model keys (#126)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 3: `IMagnifierModel` interface + `RenderModel` adapter

Define the interface that abstracts the per-tick lifecycle + draw `RunTick` performs, and wrap the existing `RenderEngine` behind it so the render path is untouched behaviorally. This task does NOT yet rewire `RunTick` (Task 4). It adds the interface and adapter and verifies they compile.

**Files:**
- Create: `src/magnifier_model.h` (interface + `PresentExtras`)
- Create: `src/render_model.h`, `src/render_model.cpp` (adapter over `RenderEngine`)

**Interfaces:**
- Produces:
  ```cpp
  namespace wind {
  // Per-tick render-only overrides RunTick computes (outline fade, inspect crosshair, click freeze,
  // cursor hide). The transform model ignores everything except drawCursor.
  struct PresentExtras {
      bool  outline = false;        // draw the edge outline this frame
      float outlineAlpha = 1.0f;    // idle-fade alpha
      bool  cursorLocked = false;   // Inspect mode: draw the crosshair at the look point
      int   cursorMode = 0;         // 0=auto,1=always,2=never (final, after cursorHidden override)
      int   clickDesktopX = 0;      // SetCursorPos target override (Inspect freeze); <INT_MIN if unset
      int   clickDesktopY = 0;
      bool  clickOverride = false;  // true when clickDesktop* should replace the mapper's click point
      bool  drawCursor = true;      // whether a cursor should be shown at all this frame
  };
  struct IMagnifierModel {
      virtual ~IMagnifierModel() = default;
      virtual bool initialize(const MonitorTarget& monitor) = 0;
      virtual void shutdown() = 0;
      virtual bool ready() const = 0;
      virtual void hideSystemCursor(bool hide) = 0;
      virtual void setActive(bool active) = 0;          // reveal/hide overlay, or enable/disable transform
      virtual void onActivate() {}                      // called on idle->active (render: invalidateCapture/prime)
      virtual bool retarget(const MonitorTarget& m) { (void)m; return false; }  // render-only; false = unchanged
      virtual void present(const MapResult& r, double level, const Config& cfg,
                           const MonitorTarget& mon, const PresentExtras& ex) = 0;  // the per-tick draw
      virtual bool coversShell() const = 0;             // render true (uiAccess band) / transform false
  };
  }
  ```
  `magnifier_model.h` includes `render_engine.h` (for `MonitorTarget`), `cursor_mapper.h` (for `MapResult`), and `config.h`. It contains NO `<windows.h>` calls itself (pure declarations), but it is a Win32-side header (not added to the test build).
- `RenderModel` (in `render_model.h`) implements `IMagnifierModel` by owning a `RenderEngine`:
  ```cpp
  class RenderModel : public IMagnifierModel {
  public:
      explicit RenderModel(int zorderBand, bool hdrTonemap);
      bool initialize(const MonitorTarget& m) override;
      void shutdown() override;
      bool ready() const override;
      void hideSystemCursor(bool hide) override;
      void setActive(bool active) override;
      void onActivate() override;
      bool retarget(const MonitorTarget& m) override;
      void present(const MapResult& r, double level, const Config& cfg,
                   const MonitorTarget& mon, const PresentExtras& ex) override;
      bool coversShell() const override;
      RenderEngine& engine();   // escape hatch for render-only main-loop code (device-lost, priming, selftest)
      bool deviceLost() const;  // forwarded (main loop calls this)
      bool recoverDeviceLost();
      void primeReveal();
      void invalidateCapture();
  private:
      RenderEngine engine_;
      int  zorderBand_;
      bool hdrTonemap_;
      bool primed_ = false;
  };
  ```

- [ ] **Step 1: Create `src/magnifier_model.h`**

Write exactly the `namespace wind { PresentExtras; IMagnifierModel; }` block from the Interfaces section above, with includes:

```cpp
#pragma once
#include "render_engine.h"   // MonitorTarget
#include "cursor_mapper.h"    // MapResult
#include "config.h"           // Config
namespace wind {
// ... PresentExtras and IMagnifierModel exactly as specified above ...
}
```

Verify `MapResult`'s type/name by reading `src/cursor_mapper.h` before writing (it is produced by `CursorMapper::update`); include the correct header.

- [ ] **Step 2: Create `src/render_model.h`**

Write the `RenderModel` class declaration from the Interfaces section, `#include "magnifier_model.h"`.

- [ ] **Step 3: Create `src/render_model.cpp`**

Implement each method by forwarding to `engine_`, reproducing exactly what `wWinMain`/`RunTick` do today:

```cpp
#include "render_model.h"

namespace wind {

// FillRenderParams currently lives in main.cpp as a static. Move it to a shared spot both main.cpp
// and this file can call: declare `void FillRenderParams(RenderFrameParams&, const MapResult&,
// const Config&, const MonitorTarget&, double);` and `int CursorModeFromCfg(const Config&);` in
// render_model.h (or a small shared header) and define them here; delete the static copies from
// main.cpp. (They are pure translation of mapper+cfg -> RenderFrameParams; no behavior change.)

RenderModel::RenderModel(int zorderBand, bool hdrTonemap)
    : zorderBand_(zorderBand), hdrTonemap_(hdrTonemap) {}

bool RenderModel::initialize(const MonitorTarget& m) { return engine_.initialize(m, zorderBand_, hdrTonemap_); }
void RenderModel::shutdown() { engine_.shutdown(); }
bool RenderModel::ready() const { return engine_.ready(); }
void RenderModel::hideSystemCursor(bool hide) { engine_.hideSystemCursor(hide); }
void RenderModel::setActive(bool active) { engine_.setVisible(active); }
void RenderModel::onActivate() { engine_.invalidateCapture(); }   // reveal/prime stays in main loop (needs ForegroundCoversMonitor)
bool RenderModel::retarget(const MonitorTarget& m) { return engine_.retarget(m); }
bool RenderModel::coversShell() const { return true; }
RenderEngine& RenderModel::engine() { return engine_; }
bool RenderModel::deviceLost() const { return engine_.deviceLost(); }
bool RenderModel::recoverDeviceLost() { return engine_.recoverDeviceLost(); }
void RenderModel::primeReveal() { engine_.primeReveal(); }
void RenderModel::invalidateCapture() { engine_.invalidateCapture(); }

void RenderModel::present(const MapResult& r, double level, const Config& cfg,
                          const MonitorTarget& mon, const PresentExtras& ex) {
    RenderFrameParams p{};
    FillRenderParams(p, r, cfg, mon, level);
    p.outline = ex.outline;
    p.outlineAlpha = ex.outlineAlpha;
    p.cursorLocked = ex.cursorLocked;
    p.cursorMode = ex.cursorMode;
    if (ex.clickOverride) { p.clickDesktopX = ex.clickDesktopX; p.clickDesktopY = ex.clickDesktopY; }
    engine_.renderFrame(p);
}
}
```

NOTE: `onActivate()` intentionally only does `invalidateCapture()`; the reveal/prime decision (`ForegroundCoversMonitor` -> `primeReveal` vs `setVisible`) stays in the main loop against `RenderModel`'s escape-hatch methods, because it is render-specific and stateful. Task 4 keeps that logic behind an `if (auto* rm = dynamic_cast<RenderModel*>(model))` guard.

- [ ] **Step 4: Compile-check**

Run: `build.bat check`
Expected: PASS (all `src\*.cpp` compile, including the new `render_model.cpp`; `main.cpp` unchanged still compiles since the new files are additive).

- [ ] **Step 5: Commit**

```bash
git add src/magnifier_model.h src/render_model.h src/render_model.cpp
git commit -m "feat(model): IMagnifierModel interface + RenderModel adapter (#126)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 4: Rewire `RunTick`/`wWinMain` onto `IMagnifierModel*` (render still default)

Swap `TickState`'s `RenderEngine&` for an `IMagnifierModel&`, route the per-tick lifecycle + draw through it, and keep the render-specific main-loop code (device-lost, reveal/prime, selftest) behind a `RenderModel` downcast. This is the highest-risk task: the render model must behave identically. Verify with the built-in self-test and a manual run.

**Files:**
- Modify: `src/main.cpp` (`TickState`, `FillRenderParams`/`CursorModeFromCfg` moved to `render_model`, `RunTick`, `wWinMain`, the two self-test blocks, the main loop, shutdown)

**Interfaces:**
- Consumes: `IMagnifierModel`, `RenderModel`, `PresentExtras` (Task 3).
- Produces: a `TickState` that holds `IMagnifierModel& model;` instead of `RenderEngine& renderEngine;`.

- [ ] **Step 1: Delete the static `FillRenderParams`/`CursorModeFromCfg` from `main.cpp`**

They now live in `render_model.cpp` (Task 3). Add their declarations to `render_model.h` so the self-test blocks in `main.cpp` (which call `FillRenderParams`) still resolve. Include `render_model.h` in `main.cpp`.

- [ ] **Step 2: Change `TickState` to hold the interface**

In `src/main.cpp`, replace `RenderEngine& renderEngine;` with `IMagnifierModel& model;`, and update the constructor:

```cpp
    IMagnifierModel& model;
    // ...
    TickState(IMagnifierModel& mdl, const MonitorTarget& m, const Config& c)
        : model(mdl), mon(m), cfg(c),
          zoom(1.0, c.maxLevel),
          mapper(m.w, m.h, c.cursorSmoothing) {}
```

- [ ] **Step 3: Route `RunTick`'s lifecycle + draw calls through `model`**

In `RunTick` (`src/main.cpp:380-580`), replace the render-engine calls:
- `t.renderEngine.retarget(nt)` -> `t.model.retarget(nt)` (line ~390).
- `t.renderEngine.hideSystemCursor(true)` -> `t.model.hideSystemCursor(true)` (lines ~402, ~419).
- `t.renderEngine.invalidateCapture()` (at activation, ~403 and ~420) -> `t.model.onActivate()` (which does `invalidateCapture` for render, no-op for transform). Keep the second `invalidateCapture` in `inspectEnter` also as `t.model.onActivate()`.
- Build a `PresentExtras ex` from the values currently poked into `p` (outline dwell/idle -> `ex.outline`/`ex.outlineAlpha`; `t.cursorHidden`/`cfg` -> `ex.cursorMode` via `CursorModeFromCfg`; inspect -> `ex.cursorLocked = true`, `ex.clickOverride`/`ex.clickDesktop*` for the freeze re-assert). Replace `FillRenderParams(p,...)` + `t.renderEngine.renderFrame(p)` (lines 502-550) with the extras computation + `t.model.present(r, lvl, t.cfg, t.mon, ex)`.
- The reveal/prime block (lines 554-570) is render-specific: guard it with a downcast:
  ```cpp
  if (auto* rm = dynamic_cast<RenderModel*>(&t.model)) {
      if (enterActive) {
          if (ForegroundCoversMonitor(t.mon)) { rm->primeReveal(); t.revealPending = 2; }
          else { rm->setActive(true); t.revealPending = 0; }
      } else if (t.revealPending > 0 && --t.revealPending == 0) {
          rm->setActive(true);
      }
  } else {
      if (enterActive) t.model.setActive(true);   // transform: reveal immediately, no capture priming
  }
  ```

Preserve every surrounding computation (outline dwell, idle fade, inspect click-freeze, `lastSetVirtual` bookkeeping) exactly; only the engine calls and the `p`-build change.

- [ ] **Step 4: Update `wWinMain` construction + main loop + selftests + shutdown**

- Replace `RenderEngine renderEngine; if (!renderEngine.initialize(...))` (`src/main.cpp:914`) with:
  ```cpp
  std::unique_ptr<IMagnifierModel> model;
  if (cfg.model == "transform") model = std::make_unique<TransformModel>(/* args, Task 9 */);
  else                          model = std::make_unique<RenderModel>(cfg.zorderBand, cfg.hdrTonemap != 0);
  if (!model->initialize(startupMon)) { /* same MessageBox + cleanup as today */ }
  ```
  (Until Task 9 lands, keep only the `RenderModel` branch so this task builds; add the `TransformModel` branch in Task 9.)
- `TickState ts(*model, startupMon, cfg);`
- The two self-test blocks (`WIND_SELFTEST`, `WIND_PACINGTEST`) call `renderEngine` directly. Guard them with `if (auto* rm = dynamic_cast<RenderModel*>(model.get()))` and use `rm->engine()` for the render-only calls (`hideSystemCursor`, `setVisible`, `renderFrame`, `dumpFrame`, `invalidateCapture`, `debugHdr`, `shutdown`). Selftests are a render-model feature; skip them for transform.
- The device-lost block in the main loop (`src/main.cpp:1056`) is render-only: guard with a downcast:
  ```cpp
  if (auto* rm = dynamic_cast<RenderModel*>(model.get()); rm && rm->deviceLost()) {
      rm->hideSystemCursor(false);
      ClipCursor(nullptr);
      if (ts.cursorLock.locked()) { ts.cursorLock.reset(); ts.clickReleaseTicks = 0; }
      unsigned long long now = GetTickCount64();
      if (now >= nextRecoverMs) {
          if (!rm->recoverDeviceLost()) nextRecoverMs = now + 500;
          else { ts.prevLvl = 1.0; ts.zoom = ZoomController(1.0, ts.cfg.maxLevel); }
      }
      Sleep(50);
      continue;
  }
  ```
- Shutdown: `renderEngine.shutdown()` (`src/main.cpp:1108`) -> `model->shutdown()`.

- [ ] **Step 5: Compile**

Run: `build.bat`
Expected: `Wind.exe` builds with 0 errors.

- [ ] **Step 6: Verify the render model is unchanged (self-test PNG)**

Run: `set WIND_SELFTEST=1 && Wind.exe` (produces `wind_selftest.png`), then unset. Open the PNG.
Expected: a magnified frame identical in kind to before this task (cursor drawn, 4x zoom). If it is blank or errors, the downcast guard around the selftest is wrong; fix before proceeding.

- [ ] **Step 7: Manual smoke of the render model**

Run `Wind.exe`, zoom in/out with the bound keys, pan, toggle Inspect mode, hit an elevated window edge.
Expected: behavior identical to `main`. This is the acceptance gate for the refactor.

- [ ] **Step 8: Commit**

```bash
git add src/main.cpp src/render_model.h
git commit -m "refactor(model): route RunTick through IMagnifierModel; render unchanged (#126)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 5: `MagnifierHost` (Magnification API wrapper)

Port Bloom's `MagnifierHost`: own the Magnification lifecycle, resolve the private `SetMagnificationDesktopMagnification` export, apply zoom/pan through it (with the public API as a permanent fallback).

**Files:**
- Create: `src/mag_host.h`, `src/mag_host.cpp`

**Interfaces:**
- Produces:
  ```cpp
  class MagHost {
  public:
      bool initialize();   // MagInitialize + resolve the private export
      // Apply zoom/pan. When fastPan and the private export resolved, uses the private channel with
      // the sub-pixel translation (tx,ty); else the public MagSetFullscreenTransform(zoom, offX, offY).
      // A private-channel failure falls back to public permanently this session.
      bool setTransform(float zoom, int offX, int offY, int tx, int ty, bool fastPan);
      void shutdown();     // public reset to 1x + MagUninitialize
  private:
      bool initialized_ = false;
      bool privateBroken_ = false;
      int  (WINAPI* setMagDesktop_)(double, int, int) = nullptr;
  };
  ```
  Consumed by Task 9.

- [ ] **Step 1: Create `src/mag_host.h`**

```cpp
#pragma once
namespace wind {
class MagHost {
public:
    bool initialize();
    bool setTransform(float zoom, int offX, int offY, int tx, int ty, bool fastPan);
    void shutdown();
private:
    bool initialized_ = false;
    bool privateBroken_ = false;
    int  (__stdcall* setMagDesktop_)(double, int, int) = nullptr;
};
}
```

- [ ] **Step 2: Create `src/mag_host.cpp`** (port of Bloom `MagnifierHost.cs`)

```cpp
#include "mag_host.h"
#include <windows.h>
#include <magnification.h>

namespace wind {

bool MagHost::initialize() {
    initialized_ = MagInitialize();
    if (initialized_) {
        HMODULE u32 = GetModuleHandleW(L"user32.dll");
        setMagDesktop_ = reinterpret_cast<int(__stdcall*)(double, int, int)>(
            u32 ? GetProcAddress(u32, "SetMagnificationDesktopMagnification") : nullptr);
    }
    return initialized_;
}

bool MagHost::setTransform(float zoom, int offX, int offY, int tx, int ty, bool fastPan) {
    if (!initialized_) return false;
    if (fastPan && !privateBroken_ && setMagDesktop_) {
        if (setMagDesktop_(zoom, tx, ty) != 0) return true;
        privateBroken_ = true;   // fall back permanently this session
    }
    return MagSetFullscreenTransform(zoom, offX, offY) != FALSE;
}

void MagHost::shutdown() {
    if (!initialized_) return;
    MagSetFullscreenTransform(1.0f, 0, 0);   // public reset restores shared state
    MagUninitialize();
    initialized_ = false;
}
}
```

`Magnification.lib` and `magnification.h` are already linked/available (the render model uses `MagShowSystemCursor`). `SetMagnificationDesktopMagnification` uses the `__stdcall`/`WINAPI` convention.

- [ ] **Step 3: Compile-check**

Run: `build.bat check`
Expected: PASS.

- [ ] **Step 4: Commit**

```bash
git add src/mag_host.h src/mag_host.cpp
git commit -m "feat(transform): MagHost - Magnification API wrapper + private pan channel (#126)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 6: `CompositionPin` (`smoothPan` present-mode hint)

Port Bloom's `CompositionPin`: a permanent 1px invisible click-through topmost layered window that keeps a foreground flip-game composited so panning does not thrash present modes.

**Files:**
- Create: `src/comp_pin.h`, `src/comp_pin.cpp`

**Interfaces:**
- Produces:
  ```cpp
  class CompositionPin {
  public:
      bool create();     // register class + create the 1px window; false on failure
      void assert_();    // show + re-assert topmost (cheap; call ~2x/s while smoothPan is active)
      void hide();
      void destroy();
  private:
      HWND hwnd_ = nullptr;
      bool visible_ = false;
  };
  ```
  Consumed by Task 9.

- [ ] **Step 1: Create `src/comp_pin.h`**

```cpp
#pragma once
#include <windows.h>
namespace wind {
class CompositionPin {
public:
    bool create();
    void assert_();
    void hide();
    void destroy();
private:
    HWND hwnd_ = nullptr;
    bool visible_ = false;
};
}
```

- [ ] **Step 2: Create `src/comp_pin.cpp`** (port of Bloom `CompositionPin.cs`)

```cpp
#include "comp_pin.h"
namespace wind {

static const wchar_t* kPinClass = L"WindCompositionPin";

bool CompositionPin::create() {
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = hInst;
    wc.lpszClassName = kPinClass;
    RegisterClassExW(&wc);   // benign if already registered (returns 0 with ERROR_CLASS_ALREADY_EXISTS)

    const DWORD exStyle = WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT
                        | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;
    hwnd_ = CreateWindowExW(exStyle, kPinClass, L"WindPin", WS_POPUP,
                            0, 0, 1, 1, nullptr, nullptr, hInst, nullptr);
    if (!hwnd_) return false;
    // Uniform 1/255 alpha: composited (so DWM keeps the game composed) but imperceptible.
    // Never 0 - DWM drops fully transparent windows.
    SetLayeredWindowAttributes(hwnd_, 0, 1, LWA_ALPHA);
    return true;
}

void CompositionPin::assert_() {
    if (!hwnd_) return;
    SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE);
    if (!visible_) { ShowWindow(hwnd_, SW_SHOWNOACTIVATE); visible_ = true; }
}

void CompositionPin::hide() {
    if (hwnd_ && visible_) { ShowWindow(hwnd_, SW_HIDE); visible_ = false; }
}

void CompositionPin::destroy() {
    hide();
    if (hwnd_) { DestroyWindow(hwnd_); hwnd_ = nullptr; }
}
}
```

- [ ] **Step 3: Compile-check**

Run: `build.bat check`
Expected: PASS.

- [ ] **Step 4: Commit**

```bash
git add src/comp_pin.h src/comp_pin.cpp
git commit -m "feat(transform): CompositionPin for smoothPan present-mode hint (#126)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 7: `CursorBlanker` (hide OS cursor by blanking the shared cursors)

Port Bloom's `CursorBlanker`: capture the standard cursor shapes, swap them for transparent ones via `SetSystemCursor`, restore via `SPI_SETCURSORS`. Exposes the original-shape map the sprite renders from.

**Files:**
- Create: `src/cursor_blanker.h`, `src/cursor_blanker.cpp`

**Interfaces:**
- Produces:
  ```cpp
  class CursorBlanker {
  public:
      CursorBlanker();                 // reload the user scheme, then snapshot originals
      const std::unordered_map<HCURSOR, HCURSOR>& originals() const;  // shared handle -> owned original-shape copy
      bool blanked() const;
      void blank();                    // swap standard cursors for transparent ones
      void restore();                  // SPI_SETCURSORS reload
  };
  ```
  Consumed by Task 8 (originals map) and Task 9.

- [ ] **Step 1: Create `src/cursor_blanker.h`**

```cpp
#pragma once
#include <windows.h>
#include <unordered_map>
namespace wind {
class CursorBlanker {
public:
    CursorBlanker();
    const std::unordered_map<HCURSOR, HCURSOR>& originals() const { return originals_; }
    bool blanked() const { return blanked_; }
    void blank();
    void restore();
private:
    std::unordered_map<HCURSOR, HCURSOR> originals_;
    bool blanked_ = false;
};
}
```

- [ ] **Step 2: Create `src/cursor_blanker.cpp`** (port of Bloom `CursorBlanker.cs`)

```cpp
#include "cursor_blanker.h"
namespace wind {

static const UINT kStandardIds[] = {
    32512, 32513, 32514, 32515, 32516, 32642, 32643,
    32644, 32645, 32646, 32648, 32649, 32650, 32651,
};

static HCURSOR CreateBlankCursor() {
    // 32x32 monochrome: AND mask all 1s (screen unchanged), XOR all 0s -> fully transparent.
    const int bytes = 32 * 32 / 8;
    BYTE andMask[bytes]; BYTE xorMask[bytes];
    memset(andMask, 0xFF, bytes);
    memset(xorMask, 0x00, bytes);
    return CreateCursor(nullptr, 0, 0, 32, 32, andMask, xorMask);
}

CursorBlanker::CursorBlanker() {
    // If a previous Wind was hard-killed while cursors were blanked, the desktop still has blank
    // shared cursors; reload the user's scheme FIRST or the blanks get captured as "originals".
    SystemParametersInfoW(SPI_SETCURSORS, 0, nullptr, 0);
    for (UINT id : kStandardIds) {
        HCURSOR shared = LoadCursorW(nullptr, MAKEINTRESOURCEW(id));
        if (!shared) continue;
        HCURSOR copy = CopyCursor(shared);
        if (copy) originals_[shared] = copy;
    }
}

void CursorBlanker::blank() {
    if (blanked_) return;
    blanked_ = true;
    for (UINT id : kStandardIds) {
        HCURSOR blank = CreateBlankCursor();
        if (blank) SetSystemCursor(blank, id);   // SetSystemCursor takes ownership of 'blank'
    }
}

void CursorBlanker::restore() {
    if (!blanked_) return;
    blanked_ = false;
    SystemParametersInfoW(SPI_SETCURSORS, 0, nullptr, 0);
}
}
```

`CopyCursor` is the Win32 macro for `CopyIcon` on `HCURSOR`. `LoadCursorW(nullptr, MAKEINTRESOURCEW(id))` matches Bloom's `LoadCursorW(0, (nint)id)`.

- [ ] **Step 3: Compile-check**

Run: `build.bat check`
Expected: PASS.

- [ ] **Step 4: Commit**

```bash
git add src/cursor_blanker.h src/cursor_blanker.cpp
git commit -m "feat(transform): CursorBlanker - blank the OS cursor safely (#126)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 8: `CursorSprite` (scene-locked cursor, incl. polarity-adaptive caret)

Port Bloom's `CursorSprite`: a topmost click-through layered window that mirrors the system cursor shape, positioned in unmagnified desktop coords so the transform magnifies it together with the content (rigidly locked). Includes the two-pass mask/inversion renderer for the I-beam caret.

**Files:**
- Create: `src/cursor_sprite.h`, `src/cursor_sprite.cpp`

**Interfaces:**
- Consumes: `CursorBlanker::originals()` (Task 7).
- Produces:
  ```cpp
  class CursorSprite {
  public:
      enum class ShapeStatus { Rendered, Hidden, Unsupported };
      explicit CursorSprite(const std::unordered_map<HCURSOR, HCURSOR>& originals);
      bool create();
      bool needsPolarity() const;
      ShapeStatus refreshShape();      // re-evaluate + repaint the sprite bitmap
      void setPolarity(bool darkCursor);
      void moveTo(int desktopX, int desktopY);   // hotspot at (x,y)
      void show();
      void hide();
      void destroy();
  };
  ```
  Consumed by Task 9.

- [ ] **Step 1: Create `src/cursor_sprite.h`**

```cpp
#pragma once
#include <windows.h>
#include <unordered_map>
namespace wind {
class CursorSprite {
public:
    enum class ShapeStatus { Rendered, Hidden, Unsupported };
    explicit CursorSprite(const std::unordered_map<HCURSOR, HCURSOR>& originals) : originals_(originals) {}
    bool create();
    bool needsPolarity() const { return needsPolarity_; }
    ShapeStatus refreshShape();
    void setPolarity(bool darkCursor);
    void moveTo(int desktopX, int desktopY);
    void show();
    void hide();
    void destroy();
private:
    void renderMaskShape(bool darkCursor);
    static const int kSize = 64;
    const std::unordered_map<HCURSOR, HCURSOR>& originals_;
    HWND    hwnd_ = nullptr;
    HCURSOR lastCursor_ = nullptr;
    ShapeStatus lastVerdict_ = ShapeStatus::Hidden;
    HICON   iconCopy_ = nullptr;
    int     hotX_ = 0, hotY_ = 0;
    bool    visible_ = false;
    bool    needsPolarity_ = false;
    bool    lastPolarityDark_ = true;
};
}
```

- [ ] **Step 2: Create `src/cursor_sprite.cpp`** (direct port of Bloom `CursorSprite.cs`)

Port each method 1:1 from Bloom's `CursorSprite.cs` (already read in full during planning). Mapping notes:
- `CreateWindowExW` with `WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW`, `WS_POPUP`, size `kSize x kSize`, class `L"WindCursorSprite"` (register with `DefWindowProcW`), in `create()`.
- `refreshShape()`: `CURSORINFO`/`GetCursorInfo`; `CURSOR_SHOWING`/`CURSOR_SUPPRESSED` gates; look up `info.hCursor` in `originals_` (miss -> `Unsupported`); `CopyIcon` the original; `GetIconInfo` for the hotspot then `DeleteObject` the mask/color bitmaps; `CreateDIBSection` a top-down 32bpp DIB; zero it; `DrawIconEx(..., DI_NORMAL)`; scan for any non-zero alpha byte; if none -> two-pass mask path (`needsPolarity_ = true; renderMaskShape(lastPolarityDark_)`); else `UpdateLayeredWindow` with an `AC_SRC_OVER`/`AC_SRC_ALPHA` `BLENDFUNCTION`, `ULW_ALPHA`. Free the previous `iconCopy_` via `DestroyIcon`, store the new one, update `hotX_/hotY_`, `lastCursor_`, `lastVerdict_`.
- `renderMaskShape(bool darkCursor)`: two DIBs filled `0xFF000000` (black) and `0xFFFFFFFF` (white); `DrawIconEx` `iconCopy_` onto each; per pixel, equal RGB -> opaque color, else luminance compare -> `ink` (`0xFF000000` dark / `0xFFFFFFFF` light) or transparent; `UpdateLayeredWindow`.
- `setPolarity(bool)`: re-render only if `needsPolarity_ && dark != lastPolarityDark_`.
- `moveTo(x,y)`: `SetWindowPos(hwnd_, nullptr, x - hotX_, y - hotY_, 0, 0, SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE)`.
- `show`/`hide`/`destroy` as in Bloom (`ShowWindow` + `DestroyWindow` + `DestroyIcon(iconCopy_)`).

C# `Span<byte>.Clear()` -> `memset(bits, 0, kSize*kSize*4)`; `Span<uint>.Fill(v)` -> a `for` loop or `std::fill_n((uint32_t*)bits, kSize*kSize, v)`. `CopyIcon((HICON)hcursor)` for `HCURSOR`. Keep every comment from the Bloom source (translated, no em-dashes).

- [ ] **Step 3: Compile-check**

Run: `build.bat check`
Expected: PASS.

- [ ] **Step 4: Commit**

```bash
git add src/cursor_sprite.h src/cursor_sprite.cpp
git commit -m "feat(transform): scene-locked CursorSprite + polarity caret (#126)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 9: `TransformModel` + wire model selection

Assemble Tasks 1 and 5-8 into an `IMagnifierModel` implementation, and construct it in `wWinMain` when `cfg.model == "transform"`. This is where the transform model runs end to end.

**Files:**
- Create: `src/transform_model.h`, `src/transform_model.cpp`
- Modify: `src/main.cpp` (add the `TransformModel` branch in the `wWinMain` model construction from Task 4; include `transform_model.h`)

**Interfaces:**
- Consumes: `MagHost` (5), `CompositionPin` (6), `CursorBlanker` (7), `CursorSprite` (8), `ComputeMagTransform` (1), `IMagnifierModel`/`PresentExtras` (3).
- Produces: `class TransformModel : public IMagnifierModel` with the ctor `TransformModel(bool fastPan, bool smoothPan, bool cursorSprite)`.

- [ ] **Step 1: Create `src/transform_model.h`**

```cpp
#pragma once
#include "magnifier_model.h"
#include "mag_host.h"
#include "comp_pin.h"
#include "cursor_blanker.h"
#include "cursor_sprite.h"
#include <memory>
namespace wind {
class TransformModel : public IMagnifierModel {
public:
    TransformModel(bool fastPan, bool smoothPan, bool useSprite)
        : fastPan_(fastPan), smoothPan_(smoothPan), useSprite_(useSprite) {}
    bool initialize(const MonitorTarget& monitor) override;
    void shutdown() override;
    bool ready() const override { return ready_; }
    void hideSystemCursor(bool hide) override;
    void setActive(bool active) override;
    void onActivate() override {}                 // no capture to prime
    void present(const MapResult& r, double level, const Config& cfg,
                 const MonitorTarget& mon, const PresentExtras& ex) override;
    bool coversShell() const override { return false; }
private:
    bool fastPan_, smoothPan_, useSprite_;
    bool ready_ = false;
    bool active_ = false;
    MonitorTarget mon_{};
    MagHost host_;
    CompositionPin pin_;
    std::unique_ptr<CursorBlanker> blanker_;
    std::unique_ptr<CursorSprite> sprite_;
    unsigned long long lastPinAssertMs_ = 0;
};
}
```

- [ ] **Step 2: Create `src/transform_model.cpp`**

```cpp
#include "transform_model.h"
#include "transform.h"   // ComputeMagTransform
#include <windows.h>

namespace wind {

bool TransformModel::initialize(const MonitorTarget& monitor) {
    mon_ = monitor;
    if (!host_.initialize()) return false;
    if (useSprite_) {
        blanker_ = std::make_unique<CursorBlanker>();
        sprite_  = std::make_unique<CursorSprite>(blanker_->originals());
        sprite_->create();
    }
    if (smoothPan_) pin_.create();
    ready_ = true;
    return true;
}

void TransformModel::hideSystemCursor(bool hide) {
    if (!useSprite_ || !blanker_) return;
    if (hide) { blanker_->blank(); if (sprite_) sprite_->show(); }
    else      { if (sprite_) sprite_->hide(); blanker_->restore(); }
}

void TransformModel::setActive(bool active) {
    active_ = active;
    if (!active) {
        host_.setTransform(1.0f, 0, 0, 0, 0, false);   // back to 1x
        pin_.hide();
    }
}

void TransformModel::present(const MapResult& r, double level, const Config& cfg,
                             const MonitorTarget& mon, const PresentExtras& ex) {
    (void)cfg; (void)mon;
    MagTransform m = ComputeMagTransform(r.srcLeft, r.srcTop, level);
    host_.setTransform((float)level, m.offX, m.offY, m.txX, m.txY, fastPan_);

    if (useSprite_ && sprite_ && ex.drawCursor) {
        CursorSprite::ShapeStatus st = sprite_->refreshShape();
        if (st == CursorSprite::ShapeStatus::Rendered) {
            // The sprite lives in unmagnified desktop coords at the cursor's click point (the same
            // point the transform maps to screen center-ish), so the transform magnifies it welded
            // to the content. clickDesktop is monitor-local; add the monitor origin for desktop px.
            sprite_->moveTo(r.clickDesktopX + mon_.x, r.clickDesktopY + mon_.y);
            sprite_->show();
            if (sprite_->needsPolarity()) sprite_->setPolarity(true);   // dark ink; polarity sampling TBD
        } else {
            sprite_->hide();   // Hidden/Unsupported: show the real (or app-custom) cursor instead
        }
    }

    if (smoothPan_ && level > 1.0) {
        unsigned long long now = GetTickCount64();
        if (now - lastPinAssertMs_ >= 500) { lastPinAssertMs_ = now; pin_.assert_(); }
    } else {
        pin_.hide();
    }
}

void TransformModel::shutdown() {
    if (sprite_) sprite_->destroy();
    if (blanker_) blanker_->restore();
    pin_.destroy();
    host_.shutdown();
    ready_ = false;
}
}
```

NOTE on polarity: Bloom samples the desktop background off the compose thread to pick caret ink polarity. For this first landing, `setPolarity(true)` (dark ink) is used unconditionally, matching Bloom's default before its polarity sampler runs. A follow-up can port the sampler; it is out of scope here (documented in the spec's "Out of scope"). Do NOT call `GetPixel` on the present path (it stalls the compositor).

NOTE on coords: `TransformModel` targets whatever monitor `initialize` was given. Task 4's `wWinMain` forces `startupMon = PrimaryMonitor()` when `model == "transform"` (next step), so `r.srcLeft/srcTop` are in primary-screen coords, which is what `MagSetFullscreenTransform` expects. `multiMonitor` is ignored for the transform model (documented per-model limitation).

- [ ] **Step 3: Wire construction in `wWinMain` (`src/main.cpp`)**

- `#include "transform_model.h"`.
- Where `startupMon` is computed (`src/main.cpp:911`), force primary for transform:
  ```cpp
  MonitorTarget startupMon = (cfg.model != "transform" && cfg.multiMonitor != 0)
                                 ? MonitorUnderCursor() : PrimaryMonitor();
  ```
- In the model construction (from Task 4), add the transform branch:
  ```cpp
  std::unique_ptr<IMagnifierModel> model;
  if (cfg.model == "transform")
      model = std::make_unique<TransformModel>(cfg.fastPan != 0, cfg.smoothPan != 0, cfg.cursorSprite != 0);
  else
      model = std::make_unique<RenderModel>(cfg.zorderBand, cfg.hdrTonemap != 0);
  ```

- [ ] **Step 4: Build**

Run: `build.bat`
Expected: `Wind.exe` builds with 0 errors.

- [ ] **Step 5: Manual verification, transform model**

Set `model=transform` in `%APPDATA%\Wind\magnifier.ini` (or the ini path Wind uses), run `Wind.exe`, then:
- Zoom in/out with the bound keys: the view magnifies via DWM (no capture overlay).
- Pan: with `fastPan=1` the pan is sub-pixel smooth; the cursor sprite stays welded to the view.
- Move to a screen edge: the cursor sprite does NOT diverge from where clicks land (the dead-zone fix).
- Hover a text field: the I-beam caret renders (polarity-adaptive dark ink).
- Set `smoothPan=1`, zoom in a borderless flip game while panning: panning is steady (capped rate) instead of stuttering.
- Set `model=render`, restart: the render model is back and unchanged.

Expected: all of the above. Acceptance bar is visual smoothness parity with Bloom's transform model and no cursor divergence at edges.

- [ ] **Step 6: Commit**

```bash
git add src/transform_model.h src/transform_model.cpp src/main.cpp
git commit -m "feat(transform): TransformModel end to end + model selection (#126)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 10: WindConfig UI (model row + value-gated visibility)

Add the model selector to the settings schema and extend `Settings.svelte`'s row-visibility logic to support a value-match gate, so transform-only and render-only rows show for the right model.

**Files:**
- Modify: `ui/src/settings-schema.js` (add the `model` row + `showIf` gates on model-specific rows)
- Modify: `ui/src/Settings.svelte` (interpret `showIf:{key, eq}`)

**Interfaces:**
- Consumes: `Config::model`/`fastPan`/`smoothPan`/`cursorSprite` (Task 2), already surfaced to the UI via the existing ini <-> schema binding.
- Produces: the `showIf` predicate, reusable by any future value-gated row.

- [ ] **Step 1: Read `ui/src/Settings.svelte` and find the row-visibility logic**

Locate where the component decides whether to render a row from `dependsOn` / `requires` / `requiresNot` (search for `dependsOn`). Note the function/expression (call it `rowVisible(row, values)`).

- [ ] **Step 2: Extend the predicate to support `showIf`**

Add to the visibility check: a row with `showIf:{ key, eq }` is visible only when `values[showIf.key] === showIf.eq` (in addition to the existing `dependsOn`/`requires`/`requiresNot` checks, all must pass). Keep the existing checks intact.

- [ ] **Step 3: Add the model row + gates in `ui/src/settings-schema.js`**

Add a `model` row (in the Display section, before the render-only quality rows, or a new top section):

```js
{ key:'model', type:'select', label:'Magnifier model',
  desc:'Render = high-fidelity GPU overlay (covers the shell). Transform = low-GPU DWM-based (restart to switch).',
  options:['render','transform'], def:'render' },
```

Add transform-only rows (new, gated):

```js
{ key:'fastPan',     type:'toggle', label:'Fast pan (sub-pixel)',
  desc:'Private DWM pan channel; auto-falls back if unavailable.', def:1, showIf:{ key:'model', eq:'transform' } },
{ key:'smoothPan',   type:'toggle', label:'Smooth pan in games',
  desc:'Keep the display composited while zoomed so flip-model games do not stutter (caps the frame rate while zoomed).', def:0, showIf:{ key:'model', eq:'transform' } },
{ key:'cursorSprite',type:'toggle', label:'Scene-locked cursor',
  desc:'Draw our own cursor welded to the view (recommended).', def:1, showIf:{ key:'model', eq:'transform' } },
```

Gate the render-only rows with `showIf:{ key:'model', eq:'render' }` by adding that property to: `bilinear`, `sharpness`, `brightness`, `hdrTonemap`, and the outline group (`outline`, `outlineThickness`, `outlineColor`, `outlineLowZoomOnly`, `outlineLowZoomMax`, `outlineIdleHide`, `outlineIdleSeconds`). Leave `multiMonitor` visible for both (render uses it; transform ignores it, documented). Do NOT gate cursor/zoom/keybind rows (shared).

- [ ] **Step 4: Build the config app**

Run: `build.bat config`
Expected: the Svelte UI builds and `WindConfig.exe` links with 0 errors.

- [ ] **Step 5: Visual verification**

Run `WindConfig.exe`. Switch the model select between `render` and `transform`.
Expected: with `transform`, the three transform rows appear and the render-only quality/outline rows hide; with `render`, the reverse. Save writes `model=`/`fastPan=`/etc. to the ini.

- [ ] **Step 6: Commit**

```bash
git add ui/src/settings-schema.js ui/src/Settings.svelte
git commit -m "feat(config-ui): model selector + value-gated model-specific rows (#126)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 11: Docs + final verification

Document the two models and the per-model limitations in the README, and run the full verification loop.

**Files:**
- Modify: `README.md`

**Interfaces:** none (docs + verification only).

- [ ] **Step 1: Add a "Magnifier models" section to `README.md`**

Read the current `README.md` structure and add a concise section documenting: `render` (default, GPU overlay, covers the shell, per-monitor) vs `transform` (low-GPU, DWM-based, `fastPan`/`smoothPan`/`cursorSprite`, restart to switch, does NOT cover the shell, primary-monitor only, flip-game pan drops fps unless `smoothPan`). Match the README's existing tone; no em-dashes.

- [ ] **Step 2: Run the full verification loop**

Run: `build.bat test`
Expected: PASS (all pure unit tests, including Tasks 1-2).

Run: `build.bat`
Expected: `Wind.exe` builds clean.

Run: `build.bat config`
Expected: `WindConfig.exe` builds clean.

Manually re-run the Task 4 render smoke AND the Task 9 transform smoke once more back to back, switching via the config app + restart.
Expected: both models work; render is unchanged from `main`.

- [ ] **Step 3: Commit**

```bash
git add README.md
git commit -m "docs: document the render and transform magnifier models (#126)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

- [ ] **Step 4: Open the PR**

```bash
git push -u origin feat/transform-model
gh pr create --title "Add a low-GPU DWM transform magnifier model (#126)" \
  --body "Adds a second selectable magnifier model ported from Bloom. render stays default. Closes #126." \
  --base main
```

---

## Self-Review

**Spec coverage:**
- Two models / why keep both -> Tasks 3, 9, 11 (docs table). Covered.
- Cursor dead-zone fix (scene-locked sprite) -> Tasks 7, 8, 9. Covered.
- Sub-pixel pan (private channel) -> Tasks 1, 5, 9. Covered.
- smoothPan composition pin -> Tasks 6, 9. Covered.
- Port not embed -> all C++ tasks; no .NET. Covered.
- Engine seam Option A (IMagnifierModel) -> Tasks 3, 4. Covered.
- Config `model` + transform keys + ini + README -> Tasks 2, 11. Covered.
- Config-app row + value-gated visibility -> Task 10. Covered.
- Restart-only switch -> Task 9 (constructed at launch; no hot-swap). Covered.
- Per-model limits (no shell, flip-game fps, primary-only, no MagSetInputTransform) -> Tasks 9, 11. Covered.
- House rules (no em-dash, pure files no windows.h) -> Global Constraints; Task 1 math is pure, all Win32 in non-test files. Covered.

**Placeholder scan:** No TBD/TODO left as work items. The two explicit deferrals (polarity sampler, multi-monitor for transform) are named as out-of-scope in the spec and stubbed deterministically (dark ink; primary-only), not left blank.

**Type consistency:** `IMagnifierModel`/`PresentExtras`/`RenderModel` signatures match between Tasks 3 and 4. `MagTransform`/`ComputeMagTransform` match between Tasks 1 and 9. `MagHost::setTransform`, `CompositionPin::assert_`, `CursorBlanker::originals`, `CursorSprite::refreshShape`/`ShapeStatus` match their consumers in Task 9. `TransformModel` ctor `(bool,bool,bool)` matches the `wWinMain` call in Task 9 Step 3.
