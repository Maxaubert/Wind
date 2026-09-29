# Colour Filters Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Invert / greyscale / warm / two-colour tint filters and a dim control, at 1x and zoomed, with an optional toggle hotkey.

**Architecture:** One DWM colour matrix (`MagSetFullscreenColorEffect`) applied through the Magnification runtime owner thread, built by a pure `color_matrix.h`, applied and lifetime-managed by `color_filter.*`, decided per tick in RunTick.

**Tech Stack:** C++17 / MSVC, Magnification API, doctest; Svelte settings UI, Playwright.

**Spec:** `docs/superpowers/specs/2026-09-29-colour-filters-design.md`

## Global Constraints

- No em-dashes anywhere (code, comments, docs, UI copy).
- Pure headers must not include `<windows.h>`.
- Every Magnification call runs on the owner thread via `wind::MagThreadInvoke`; the runtime is held only through `MagApiAcquire`/`MagApiRelease`.
- Nothing may cost anything when no filter is on and dim is 100% (no context, no per-tick syscalls).
- Identity must be restored on every exit path (disable, toggle, shutdown, crash filter, atexit).
- Branch `feat/288-colour-filters`, commits `type(scope): subject` with the attribution trailer.

## Review Focus

1. Wind killed (Task Manager) while a filter is on: the screen must not stay filtered (spike decides the mechanism).
2. Zoom in and out with a filter on: no flash of unfiltered or double-filtered frames at the transitions.
3. Render engine while zoomed: the magnified picture filtered exactly once.
4. HDR desktop: the filter still looks right (invert of HDR white).
5. Filter at 1x plus a game that toggles its cursor: the known context tax, only while a 1x filter is on.

---

### Task 1: Spike (throwaway probe, not committed)

**Files:** scratchpad only.

- [ ] Probe exe: `MagInitialize`, `MagSetFullscreenColorEffect(invert)` at level 1; read a known white pixel through Desktop Duplication before and after. Records whether DDA sees the effect (decides 4.1 single vs render-shader path).
- [ ] Same probe, then `TerminateProcess` itself with the effect on: does the screen return to normal? (decides 4.4 crash handling).
- [ ] Repeat the pixel read with HDR on, if the display supports it.
- [ ] Record results in the spec section 4.1 / 4.4 and in `docs/COLOUR-FILTER-FINDINGS.md`.

### Task 2: Pure matrices

**Files:** Create `src/color_matrix.h`, `tests/test_color_matrix.cpp`.

**Interfaces (produces):** `enum class ColorFilter { Off=0, Invert, Greyscale, Warm, YellowOnBlack, WhiteOnBlue, GreenOnBlack };` `struct ColorMatrix { float m[5][5]; };` `ColorMatrix BuildColorMatrix(ColorFilter f, double warm01, double dim01);` `bool IsIdentity(const ColorMatrix&);` `ColorMatrix Multiply(const ColorMatrix&, const ColorMatrix&);` `RGB ApplyToRgb(const ColorMatrix&, double r, double g, double b)` (test helper).

- [ ] Write the tests: identity for (Off, any, 1.0); invert maps white to black and black to white; greyscale maps pure red to 0.2126 grey; warm strength 1 keeps red, reduces blue to 0.4; yellow-on-black maps white page to black background and black text to yellow; dim 0.5 halves; Invert + dim composes.
- [ ] Run `build.bat test`, see them fail; implement; see them pass.
- [ ] Commit `feat(color): pure colour matrices (#288)`.

### Task 3: Config

**Files:** `src/config.h`, `src/config.cpp`, `tests/test_config.cpp`.

- [ ] Keys: `colorFilter` (0-6, default 0), `colorWarmPct` (10-100, default 50), `colorDimPct` (20-100, default 100), `colorAt1x` (default 1), `colorToggleVk`/`colorToggleMods` (default 0 = no hotkey). Parse, clamp, template lines.
- [ ] Tests: defaults, parse, clamps.
- [ ] Commit `feat(config): colour filter keys (#288)`.

### Task 4: Controller

**Files:** Create `src/color_filter.h`, `src/color_filter.cpp`.

**Interfaces:** `class ColorFilterController { void apply(const ColorMatrix& want, bool needOwnHold); void shutdown(); }` plus a static `RestoreColorIdentityForCrash()`.

- [ ] `apply`: if `want` equals the last applied matrix and the hold state matches, return (no syscalls). Else take or drop the own `MagApiAcquire` hold as `needOwnHold` says, then `MagThreadInvoke` a `MagSetFullscreenColorEffect`. Identity + no hold = fully released.
- [ ] `shutdown` and the crash restore write identity (while a runtime exists) and release.
- [ ] Log each change once (`color` tag): filter, dim, hold.
- [ ] Commit `feat(color): colour filter controller (#288)`.

### Task 5: RunTick, hotkey, exits

**Files:** `src/main.cpp`.

- [ ] Per tick: `want = BuildColorMatrix(cfg, toggleOn)`; identity if the model is `magnify`, or if not zoomed and `colorAt1x` is 0. `needOwnHold = !IsIdentity(want) && !zoomed`. Call the controller. When the spike says the render engine filters twice, clear the effect while a render session is active and pass the matrix to the render model instead (Task 5b).
- [ ] Hotkey: register `colorToggleVk/Mods` with `RegisterHotKey` like the hide-cursor hotkey (hot-reload on change); WM_HOTKEY flips `colorToggleOn`.
- [ ] Exit paths: controller `shutdown()` in the normal shutdown, the crash filter and `atexit` next to `RestoreInputState`; a model switch re-applies.
- [ ] (5b, only if the spike requires it) render shader: add a 5x5 matrix to the constant buffer and apply it after the brightness stage.
- [ ] Commit `feat(color): colour filters in RunTick + toggle hotkey (#288)`.

### Task 6: Settings UI

**Files:** `ui/src/settings-schema.js`, `ui/tests/settings.spec.js`.

- [ ] New section `{ id:'colour', label:'Colour' }`: select `colorFilter` (Off, Invert, Greyscale, Warm, Yellow on black, White on blue, Green on black), slider `colorWarmPct` (%), slider `colorDimPct` (%, "100% = normal"), toggle `colorAt1x`, keybind `__colorToggle` (vkKey `colorToggleVk`, modsKey `colorToggleMods`).
- [ ] Playwright: the section renders, choosing Invert writes `colorFilter=1`, dim slider writes `colorDimPct`.
- [ ] Commit `feat(ui): Colour settings section (#288)`.

### Task 7: Verify, docs, ship

- [ ] `build.bat test`, Playwright, deploy via `tools\uiaccess_setup.ps1`; owner field test per spec section 5.
- [ ] Docs: `docs/COLOUR-FILTER-FINDINGS.md` (spike results), a section in `docs/architecture/07-cursor.md` or a new chapter entry, README feature line, CLAUDE.md gotcha (effect lifetime / crash restore).
- [ ] Version bump in the PR; review workflow (sonnet reviewers + verifier), owner approves fixes, PR, owner says merge.
