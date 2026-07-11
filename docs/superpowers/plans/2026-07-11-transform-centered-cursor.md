# Transform centered cursor Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Center the transform model's drawn cursor (sprite at screen center, view pans under it) by decoupling the drawn cursor from the OS cursor, with a per-tick anchored fallback whenever a visible cursor exists we cannot place.

**Architecture:** The mapper already computes everything: the centered clamped source rect (`srcLeft/srcTop`), the lens center (`clickDesktopX/Y` = C, where the OS cursor is welded and clicks land), and `cursorScreenX/Y = (center - src) * level` = `T(C)`, the screen point showing the lens-center content. Centered mode feeds the transform the mapper's rect and draws the sprite at `cursorScreen`; anchored mode (today's `ComputeFixedPointOffset`) remains for `transformCenterCursor=0`, `cursorSprite=0`, and app-custom cursor shapes that cannot be blanked. Spec: `docs/superpowers/specs/2026-07-11-transform-centered-cursor-design.md`.

**Tech Stack:** C++17 / MSVC (`build.bat`), doctest (`build.bat test`), Svelte config UI (`build.bat config`).

## Global Constraints

- No em-dashes (U+2014) anywhere: code, comments, docs, UI copy. Use en-dashes, commas, or rephrase.
- Pure-logic files (`config.cpp`, `cursor_mapper.cpp`) MUST NOT include `<windows.h>`; they compile in the `WIND_TESTS` build.
- The render model is untouched. `ComputeFixedPointOffset` stays (anchored fallback).
- `transformCenterCursor` defaults to **1**, is hot-reloadable (read from `cfg` per tick in `present()`, no constructor wiring), transform-only.
- Feature work lands as one GitHub issue -> branch `feat/transform-centered-cursor` -> PR. Live click verification on the deployed build is the merge gate.

---

### Task 1: `transformCenterCursor` config key

**Files:**
- Modify: `src/config.h` (after the `cursorSprite` field, line 76-78)
- Modify: `src/config.cpp` (parse after the `cursorSprite` line 114; default-ini template after `cursorSprite=1` line 270)
- Test: `tests/test_config_model.cpp`

**Interfaces:**
- Produces: `Config::transformCenterCursor` (`int`, default `1`), parsed from ini key `transformCenterCursor`. Task 3 reads it via the `cfg` parameter of `present()`.

- [ ] **Step 1: Write the failing test**

Append to `tests/test_config_model.cpp`:

```cpp
TEST_CASE("transformCenterCursor parses and defaults on") {
    CHECK(ParseConfig("").transformCenterCursor == 1);
    CHECK(ParseConfig("transformCenterCursor=0\n").transformCenterCursor == 0);
    CHECK(ParseConfig("transformCenterCursor=1\n").transformCenterCursor == 1);
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `build.bat test`
Expected: FAIL to compile - `transformCenterCursor` is not a member of `wind::Config`.

- [ ] **Step 3: Add the field to `src/config.h`**

Immediately after the `cursorSprite` field block (`int cursorSprite = 1; // ...` and its comment continuation lines), add:

```cpp
    int transformCenterCursor = 1; // 1 = center the drawn cursor (sprite) and pan the view under it,
                                   //     falling back per tick to cursor-anchored whenever a visible
                                   //     cursor exists we cannot place (app-custom shape, cursorSprite=0).
                                   // 0 = always cursor-anchored (the free cursor). Hot-reloadable.
```

- [ ] **Step 4: Parse the key in `src/config.cpp`**

After `else if (key == "cursorSprite")       c.cursorSprite = std::stoi(val);` (line 114), add:

```cpp
            else if (key == "transformCenterCursor") c.transformCenterCursor = std::stoi(val);
```

- [ ] **Step 5: Add it to the default-ini template in `src/config.cpp`**

After the `"cursorSprite=1\n"` template line (line 270), add:

```cpp
               "; transformCenterCursor (transform only): 1 = keep the drawn cursor centered and pan\n"
               ";   the view under it (needs cursorSprite=1); 0 = free cursor anchored to the magnification\n"
               "transformCenterCursor=1\n"
```

- [ ] **Step 6: Run the test to verify it passes**

Run: `build.bat test`
Expected: PASS (exit 0).

- [ ] **Step 7: Commit**

```bash
git add src/config.h src/config.cpp tests/test_config_model.cpp
git commit -m "feat(config): add transformCenterCursor toggle (default on)"
```

### Task 2: pin the mapper invariant centered mode relies on

**Files:**
- Test: `tests/test_cursor_mapper.cpp`

No production code changes. This documents-by-test the algebra Task 3's sprite placement depends on: `cursorScreen == (center - src) * level` under the clamped centered rect, i.e. `cursorScreen` is exactly `T(lens center)`.

**Interfaces:**
- Consumes: `CursorMapper(int, int, double)`, `reset(double, double)`, `update(int, int, double)` returning `MapResult` - all existing.
- Produces: nothing consumed by later tasks (documentation test).

- [ ] **Step 1: Write the test (passes immediately - it pins existing behavior)**

Append to `tests/test_cursor_mapper.cpp`:

```cpp
TEST_CASE("invariant: cursorScreen is T(center) under the clamped centered rect "
          "(the transform model's centered mode places the sprite there)") {
    CursorMapper m(1000, 800, 0.0);   // smoothing 0 = snap, so reset() fully determines the center
    const double L = 4.0;
    // Steady mid-screen: the rect centers, so the cursor sits at the screen center.
    m.reset(500, 400);
    MapResult r = m.update(0, 0, L);
    CHECK(r.cursorScreenX == doctest::Approx((r.centerX - r.srcLeft) * L));
    CHECK(r.cursorScreenY == doctest::Approx((r.centerY - r.srcTop) * L));
    CHECK(r.cursorScreenX == doctest::Approx(500.0));
    CHECK(r.cursorScreenY == doctest::Approx(400.0));
    // Clamped corner: the rect stops at the desktop edge; cursorScreen slides toward the
    // corner but the identity cursorScreen == (center - src) * level still holds, so a sprite
    // drawn there still sits on the lens-center content.
    m.reset(999, 799);
    r = m.update(0, 0, L);
    CHECK(r.cursorScreenX == doctest::Approx((r.centerX - r.srcLeft) * L));
    CHECK(r.cursorScreenY == doctest::Approx((r.centerY - r.srcTop) * L));
    CHECK(r.cursorScreenX > 900.0);   // pushed near the right edge, no longer centered
    CHECK(r.cursorScreenY > 700.0);
}
```

- [ ] **Step 2: Run the test to verify it passes**

Run: `build.bat test`
Expected: PASS. (Sanity of the corner numbers: at L=4 on 1000px, the rect is 250px wide, max srcLeft is 750; center 999 gives cursorScreen (999-750)*4 = 996 > 900.)

- [ ] **Step 3: Commit**

```bash
git add tests/test_cursor_mapper.cpp
git commit -m "test(mapper): pin cursorScreen == T(center), the centered-mode contract"
```

### Task 3: centered present path in the transform model

**Files:**
- Modify: `src/transform_model.cpp` (the whole `present()` body, lines 35-102)

No unit test (needs `<windows.h>`; not in the test build). Verified by `build.bat` + the live deploy gate.

**Interfaces:**
- Consumes: `Config::transformCenterCursor` (Task 1); existing `MapResult` fields `srcLeft/srcTop`, `cursorScreenX/Y`, `clickDesktopX/Y`, `centerX/Y`; existing `CursorSprite` API (`refreshShape`, `showCrosshair`, `moveTo`, `show`, `hide`, `keepOnTop`); `ComputeFixedPointOffset`, `ComputeMagTransform`.
- Produces: nothing consumed by later tasks.

- [ ] **Step 1: Replace the `present()` body**

Replace everything between `void TransformModel::present(...) {` and its closing brace (currently lines 35-102) with:

```cpp
void TransformModel::present(const MapResult& r, double level, const Config& cfg,
                             const MonitorTarget& mon, const PresentExtras& ex) {
    (void)mon;
    // Resolve the sprite verdict FIRST: it decides which offset this tick uses. During Inspect the
    // sprite is our crosshair (always placeable), so refreshShape is only consulted for the normal
    // cursor path.
    const bool inspect = ex.cursorLocked;
    CursorSprite::ShapeStatus st = CursorSprite::ShapeStatus::Hidden;
    if (useSprite_ && sprite_ && ex.drawCursor && !inspect) st = sprite_->refreshShape();

    // CENTERED vs ANCHORED (spec: docs/superpowers/specs/2026-07-11-transform-centered-cursor-design.md).
    // DWM composites the cursor AND layered windows OUTSIDE the fullscreen magnification (measured:
    // a layered window at desktop P draws at screen P, unscaled), so the two geometries are:
    //  - CENTERED (the render model's): source rect = the mapper's centered clamped rect; the sprite
    //    is drawn at cursorScreen = T(C) (screen center in steady state, edge-shifted at clamps);
    //    the OS cursor is welded to the lens center C, so a click lands exactly on the content under
    //    the sprite. Requires that the visible cursor is one WE place (the sprite or the Inspect
    //    crosshair), or that no cursor is visible at all (drawCursor off / shape Hidden - the real
    //    cursor is blanked or app-suppressed).
    //  - ANCHORED: off = L*(1 - 1/level) makes T(L) == L, so a real cursor drawn by DWM at its own
    //    desktop spot sits on exactly what it clicks. Used when an app-custom shape (Unsupported)
    //    cannot be blanked, when cursorSprite=0, and when transformCenterCursor=0.
    // A standard<->custom shape transition steps the view once; rare and correct.
    const bool centered = cfg.transformCenterCursor != 0 && useSprite_ && sprite_
                       && (inspect || !ex.drawCursor || st != CursorSprite::ShapeStatus::Unsupported);

    OffsetF src = centered ? OffsetF{ r.srcLeft, r.srcTop }
                           : ComputeFixedPointOffset(r.centerX, r.centerY, level);
    MagTransform m = ComputeMagTransform(src.x, src.y, level);
    host_.setTransform((float)level, m.offX, m.offY, m.txX, m.txY, fastPan_);

    // Weld the hidden OS cursor to the lens point, exactly as RenderEngine::render does. This keeps
    // clicks landing at the lens center AND keeps RunTick's warp-and-measure pan tracking consistent
    // (RunTick assumes the cursor was moved here each active tick). Deduped so an idle tick injects
    // no synthetic mouse move. Inspect freeze pins the point via ex.clickOverride; otherwise
    // clickDesktop is monitor-local, so add the monitor origin for desktop px.
    int cx = ex.clickOverride ? ex.clickDesktopX : (r.clickDesktopX + mon_.x);
    int cy = ex.clickOverride ? ex.clickDesktopY : (r.clickDesktopY + mon_.y);
    if (!haveLastClick_ || cx != lastClickX_ || cy != lastClickY_) {
        SetCursorPos(cx, cy);
        lastClickX_ = cx; lastClickY_ = cy; haveLastClick_ = true;
    }

    // Where the drawn cursor goes on SCREEN. Centered: cursorScreen (= T(C), the screen point that
    // shows the lens-center content - the sprite sits on exactly what a click at C hits). Anchored:
    // the click point itself (T(L) == L there). The layered sprite composites unmagnified, so its
    // window position IS its screen position.
    const int sx = centered ? (int)(r.cursorScreenX + 0.5) + mon_.x : cx;
    const int sy = centered ? (int)(r.cursorScreenY + 0.5) + mon_.y : cy;

    if (useSprite_ && sprite_ && inspect && ex.drawCursor) {
        // Inspect mode: the real cursor is frozen at the (overridden) click point, but the thing the
        // user aims with is the LOOK POINT (mapper center). Repaint the sprite as the crosshair (the
        // same design the render model draws) and put it on the look point's SCREEN position:
        // cursorScreen under the centered rect; with the anchored offset T(L) == L makes that the
        // look point's desktop position itself. NOT cx/cy - those are pinned to the frozen cursor.
        sprite_->showCrosshair();
        sprite_->moveTo(centered ? sx : r.clickDesktopX + mon_.x,
                        centered ? sy : r.clickDesktopY + mon_.y);
        sprite_->keepOnTop();
    } else if (useSprite_ && sprite_ && ex.drawCursor) {
        if (st == CursorSprite::ShapeStatus::Rendered) {
            sprite_->moveTo(sx, sy);
            sprite_->show();
            // Composited outside the magnification, so it must fight for real z-order: reclaim the top
            // of our band when a popup (tray/context menu, flyout) has been raised over us. Throttled.
            sprite_->keepOnTop();
        } else {
            sprite_->hide();   // Hidden/Unsupported: show the real (or app-custom) cursor instead
        }
    } else if (useSprite_ && sprite_) {
        // cursorVisibility=never, or the hide-cursor hotkey. The block above is what MOVES the sprite,
        // so skipping it is not enough: hideSystemCursor(true) already showed the sprite at activation
        // and it would freeze on screen, visible and no longer tracking. Hide it explicitly. hide() is
        // idempotent, and the real cursor stays blanked (CursorBlanker is independent of this flag),
        // so nothing unmagnified reappears; zoom-out restores it via hideSystemCursor(false).
        sprite_->hide();
    }

    if (smoothPan_ && level > 1.0) {
        unsigned long long now = GetTickCount64();
        if (now - lastPinAssertMs_ >= 500) { lastPinAssertMs_ = now; pin_.assert_(); }
    } else {
        pin_.hide();
    }
}
```

Notes for the implementer:
- The old body's `(void)cfg;` is gone on purpose - `cfg.transformCenterCursor` is now read per tick, which is what makes the toggle hot-reloadable with zero wiring.
- In the Inspect branch, the anchored arm draws at `r.clickDesktopX + mon_.x` (the look point), NOT at `cx` (the frozen point) - this preserves the exact behavior shipped in PR #138.
- `r.cursorScreenX/Y` is monitor-local screen px and always >= 0 (clamped rect), so `(int)(v + 0.5)` rounding is safe.

- [ ] **Step 2: Build and verify it compiles**

Run: `build.bat`
Expected: exit 0, `Wind.exe` produced, zero warnings. (An "OffsetF: no appropriate default constructor" style error means the aggregate init `OffsetF{ r.srcLeft, r.srcTop }` was altered - keep it as brace init.)

- [ ] **Step 3: Run the full test suite (regression)**

Run: `build.bat test`
Expected: PASS (exit 0) - present() is not in the test build, this catches accidental damage to pure files.

- [ ] **Step 4: Commit**

```bash
git add src/transform_model.cpp
git commit -m "feat(transform): centered cursor - decouple the drawn cursor from the OS cursor"
```

### Task 4: config UI toggle

**Files:**
- Modify: `ui/src/settings-schema.js` (Display section, immediately after the `cursorSprite` row at line 39-41)

**Interfaces:**
- Consumes: the existing `toggle` row type, `dependsOn`, `showIf`.
- Produces: a "Center cursor" row writing ini key `transformCenterCursor` (hot-reloaded by the core).

- [ ] **Step 1: Add the schema row**

In `ui/src/settings-schema.js`, immediately after the `cursorSprite` row object:

```js
    { key:'cursorSprite',type:'toggle', label:'Scene-locked cursor',
      desc:'Draw our own cursor welded to the view (recommended).', def:1, showIf:{ key:'model', eq:'transform' } },
```

insert:

```js
    { key:'transformCenterCursor', type:'toggle', label:'Center cursor',
      desc:'Keep the cursor centered while the view pans under it. Off = free cursor anchored to the magnification.',
      def:1, dependsOn:'cursorSprite', showIf:{ key:'model', eq:'transform' } },
```

- [ ] **Step 2: Build the config UI**

Run: `build.bat config`
Expected: exit 0; `WindConfig.exe` + `ui/dist/` produced. (No `Settings.svelte` change needed: plain schema rows load their value/default via the generic loop; only keybind rows need `kbDefaults`.)

- [ ] **Step 3: Commit**

```bash
git add ui/src/settings-schema.js
git commit -m "feat(config): Center cursor toggle for the transform model"
```

### Task 5: document the centered cursor

**Files:**
- Modify: `README.md` (the transform-model/settings description - search for `cursorSprite` or "Transform")
- Modify: `CLAUDE.md` (IMPORTANT gotchas section)

- [ ] **Step 1: Update `README.md`**

Where the transform model's behavior/knobs are described, add one line: the transform model keeps the drawn cursor centered while the view pans under it (`transformCenterCursor=1`, default; needs the scene-locked cursor sprite); set 0 for the previous free cursor, anchored to the magnification.

- [ ] **Step 2: Add the gotcha to `CLAUDE.md`**

In the IMPORTANT gotchas section, after the INPUT SWALLOWING entry, add a bullet:

```markdown
- TRANSFORM MODEL CURSOR GEOMETRY: DWM composites the cursor AND layered windows OUTSIDE the
  fullscreen magnification (measured, PR #130), so the drawn cursor's window position IS its screen
  position. Two self-consistent geometries exist and `transform_model.cpp` picks per tick:
  CENTERED (default, `transformCenterCursor=1` + the sprite): source rect = the mapper's centered
  clamped rect, sprite at `cursorScreen` (= T(C)), OS cursor welded to the lens center C - clicks
  land exactly under the sprite. ANCHORED fallback (`ComputeFixedPointOffset`, T(L) == L): used for
  app-custom cursor shapes that cannot be blanked, `cursorSprite=0`, or `transformCenterCursor=0` -
  the ONLY geometry where a cursor drawn by DWM at its own desktop spot is correct. Do NOT weld the
  sprite to the OS cursor position under a centered rect (that was the original click-drift bug,
  issue #129), and do NOT scale the sprite with zoom (the failed `044257f` attempt).
```

- [ ] **Step 3: Commit**

```bash
git add README.md CLAUDE.md
git commit -m "docs: document the transform centered cursor + geometry gotcha"
```

---

## Self-Review notes

- **Spec coverage:** config key + default -> Task 1; mapper invariant test -> Task 2; centered offsets, sprite at cursorScreen, weld unchanged, mode-selection table, refreshShape order swap, Inspect crosshair position -> Task 3; UI toggle (dependsOn cursorSprite, transform-only) -> Task 4; docs -> Task 5. Manual verification steps live in the spec's Testing section and run at the deploy gate before the PR merges.
- **Mode table check vs Task 3 code:** Rendered -> centered (st != Unsupported); drawCursor false -> centered (`!ex.drawCursor` arm); Hidden -> centered (st != Unsupported); Unsupported -> anchored; cursorSprite=0 -> anchored (`useSprite_` in the conjunction); toggle=0 -> anchored. Inspect -> centered (inspect arm). Matches the spec table.
- **Type consistency:** `OffsetF{ r.srcLeft, r.srcTop }` matches `struct OffsetF { double x; double y; }` (aggregate). `MapResult` field names match `cursor_mapper.h`. `ShapeStatus` enum values match `cursor_sprite.h`. `Config::transformCenterCursor` name identical across Tasks 1, 3, 4.
