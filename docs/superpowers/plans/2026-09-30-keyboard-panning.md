# Keyboard Panning Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans (native) to implement
> this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** While zoomed, Ctrl+Alt+arrows (rebindable) nudge on tap and pan on hold, like Windows Magnifier.

**Architecture:** A pure `KeyPan` motion model feeds a new `ViewOwner::Keys` on the existing
detached-view path (#276). The keyboard hook swallows pan keys only while the tick says "armed"
(zoomed), so at 1x they reach the app.

**Tech Stack:** C++17 (MSVC), doctest, Svelte + Playwright.

**Spec:** `docs/superpowers/specs/2026-09-30-keyboard-panning-design.md`

## Global Constraints
- No em-dashes anywhere (code, comments, docs, UI copy).
- Pure files (`keyboard_pan.h`, `view_target.h`, `config.cpp` parse half) never include `<windows.h>`.
- Defaults: pan keys 37/39/38/40 with mods 3 (Ctrl+Alt); `panSpeed` 1.0, range 0.25-4.
- Nudge = 1/8 screen; hold threshold 250 ms; continuous = `panSpeed x 0.5` screens/s; ease-in ~150 ms; glide-out ~120 ms.
- Swallow decision once per press (existing `firstDown` rule); pan slots match only while armed.
- Version 0.15.0. Live checks on the owner's PC only while it is idle (GetLastInputInfo > 60 s).

## Review Focus
1. A pan key pressed at 1x, then the user zooms while still holding it: must not be swallowed mid-press (the app saw the DOWN), must not strand the key.
2. Zoom out while a pan key is held: the swallowed UP stays balanced; panning stops; the view returns to the mouse path at 1x.
3. Panning at high zoom toward the bottom-right with nearest sampling and MPO on: the centre must respect the MPO wall (never produce |tx| past the #148 limit).
4. IntelliJ at 1x: Ctrl+Alt+Left/Right must reach it (armed flag false).
5. Mouse move after a pan: pointer placed in the view, no jump of the view; a click after a pan (button, no move) gives the view back without warping.

---

### Task 1: Pure motion model `KeyPan`
**Files:** Create `src/keyboard_pan.h`, `tests/test_keyboard_pan.cpp`.
**Produces:** `struct KeyPan { void step(const bool held[4], double dtMs, double level, int monW, int monH, double speed, double& dx, double& dy); bool active() const; void reset(); }`; index order Left, Right, Up, Down.

- [ ] Write tests: tap (held one tick of 7 ms then released, stepped 1 s) moves exactly `monW/8/level` left (+-1%) at level 2 and 8; hold 2 s at speed 1 moves ~`nudge + (2-0.25) x 0.5 x monW / level` minus ease terms (assert within 10%); after release `active()` becomes false within 0.6 s; Left+Up moves both axes; Left+Right held gives 0 continuous motion (the two nudges cancel); speed 2 is ~2x the continuous distance of speed 1.
- [ ] Run `build.bat test`, expect FAIL (missing header).
- [ ] Implement:
```cpp
#pragma once
// Keyboard panning (issue #287): tap = one nudge (1/8 screen), hold > 250 ms = continuous pan with
// ease-in, release glides out. Screen-space rates, returned as desktop-pixel deltas. Pure.
#include <cmath>
namespace wind {
struct KeyPan {
    static constexpr double kNudgeFrac = 0.125, kHoldMs = 250.0, kScreensPerSec = 0.5;
    static constexpr double kEaseInMs = 150.0, kGlideMs = 120.0;
    double heldMs[4] = {0, 0, 0, 0};
    double nudgeX = 0, nudgeY = 0;   // screen px still to travel from nudges
    double vx = 0, vy = 0;           // continuous velocity, screen px/s
    void reset() { *this = KeyPan{}; }
    bool active() const {
        return heldMs[0] > 0 || heldMs[1] > 0 || heldMs[2] > 0 || heldMs[3] > 0 ||
               std::fabs(nudgeX) > 0.5 || std::fabs(nudgeY) > 0.5 || std::fabs(vx) > 1 || std::fabs(vy) > 1;
    }
    void step(const bool held[4], double dtMs, double level, int monW, int monH, double speed,
              double& dx, double& dy) {
        if (level < 1.0) level = 1.0;
        const double sgn[4] = { -1, 1, -1, 1 };
        double tvx = 0, tvy = 0;
        for (int i = 0; i < 4; ++i) {
            const bool horiz = i < 2;
            if (held[i]) {
                if (heldMs[i] == 0) (horiz ? nudgeX : nudgeY) += sgn[i] * kNudgeFrac * (horiz ? monW : monH);
                heldMs[i] += dtMs > 0 ? dtMs : 0.001;
                if (heldMs[i] >= kHoldMs) (horiz ? tvx : tvy) += sgn[i] * speed * kScreensPerSec * (horiz ? monW : monH);
            } else heldMs[i] = 0;
        }
        const double tau = (std::fabs(tvx) + std::fabs(tvy) > 0) ? kEaseInMs / 3 : kGlideMs / 3;
        const double a = 1.0 - std::exp(-dtMs / tau);
        vx += (tvx - vx) * a; vy += (tvy - vy) * a;
        const double g = 1.0 - std::exp(-dtMs / (kGlideMs / 3));
        const double nx = nudgeX * g, ny = nudgeY * g;
        nudgeX -= nx; nudgeY -= ny;
        dx = (nx + vx * dtMs / 1000.0) / level;
        dy = (ny + vy * dtMs / 1000.0) / level;
        if (std::fabs(vx) < 1 && tvx == 0) vx = 0;
        if (std::fabs(vy) < 1 && tvy == 0) vy = 0;
    }
};
}  // namespace wind
```
- [ ] Run tests, expect PASS (tune constants only if a test shows the feel is off; keep the spec's numbers).
- [ ] Commit `feat(pan): pure keyboard-pan motion model (#287)`.

### Task 2: Config keys and rules
**Files:** Modify `src/config.h`, `src/config.cpp` (parse, clamp `panSpeed`, sanitise the four binds with `CheckKeyBind`, ini template block), `tests/test_config.cpp`, `tests/fixtures/keybind_cases.txt`.
**Produces:** `Config::panLeftVk, panLeftMods, panRightVk, panRightMods, panUpVk, panUpMods, panDownVk, panDownMods` (int), `Config::panSpeed` (double).

- [ ] Tests: `ParseConfig("")` gives 37/3, 39/3, 38/3, 40/3 and 1.0; `panSpeed=9` clamps to 4; `panLeftVk=65\npanLeftMods=0` (bare A) reads as 0; fixture lines `key 25 3 ok`, `key 26 3 ok`, `key 27 3 ok`, `key 28 3 ok`.
- [ ] Implement, run `build.bat test`, commit `feat(config): pan keys and pan speed (#287)`.

### Task 3: `ViewOwner::Keys`
**Files:** Modify `src/view_target.h`, `tests/test_view_target.cpp`.
**Produces:** `enum class ViewOwner { Mouse, Caret, Focus, Keys }`; `ViewOwnerInputs::panning` (bool).

- [ ] Tests: `panning=true` with no mouse motion makes owner Keys; mouse movement of 3 px then sets owner Mouse with `warpPointer`; a button press returns Mouse without `warpPointer`; `enabled=false` resets to Mouse; with panning and a gated caret event in the same tick, Keys wins (explicit request beats the caret).
- [ ] Implement: after the mouse block, `if (in.panning) { s.owner = ViewOwner::Keys; s.lastSeq = in.snap.seq; return s.owner; }`. Update the `kName` log table in main.cpp to include "keys".
- [ ] Run tests, commit `feat(view): Keys owner for keyboard panning (#287)`.

### Task 4: Hook arming
**Files:** Modify `src/input_router.h`, `src/input_router.cpp`.
**Produces:** `void setPanKeys(const int vk[4], const int mods[4]); void setPanArmed(bool);` pan slots in `isBoundKey` and, while armed, in `keyBindMatches`.

- [ ] Implement with atomics like the zoom slots; `setPanKeys` clears pressed/swallowed records only for VKs that changed (same as `setKeys`).
- [ ] Build (`build.bat`), commit `feat(input): pan keys swallowed only while zoomed (#287)`.

### Task 5: Tick wiring
**Files:** Modify `src/main.cpp`.
- [ ] At start and on hot-reload: `g_input.setPanKeys(...)`.
- [ ] Each tick: `g_input.setPanArmed(lvl > 1.001 && !t.model->selfDrivenZoom() && !inspect && !t.detector.locked())` (published before the view block).
- [ ] Read `held[4]` via `comboHeld(t.cfg.panLeftVk, t.cfg.panLeftMods)` etc. only while armed; step `t.keyPan` with the tick dt (clamped to 50 ms); `vi.panning = armed && t.keyPan.active()`.
- [ ] Owner logic enabled = `trackEnabled || panEnabled` where `panEnabled = lvl > 1.001 && !panel && !inspect && !t.detector.locked()`; keep `g_track.setActive(trackEnabled, ...)` unchanged.
- [ ] Owner Keys branch: seed `viewCx/Cy` from `r` when coming from Mouse; add the KeyPan delta; clamp the centre to `[w/(2 lvl), w - w/(2 lvl)]` (same for y) and, when `wallNeeded`, to the MPO wall as mouse edge mode does; `r = DetachedMap(...)`, `t.mapper.reset`, `t.lastSetVirtual = cur`, `t.viewDetached = true`.
- [ ] `t.keyPan.reset()` on zoom-out to idle and when disarmed.
- [ ] Build, deploy (`tools\uiaccess_setup.ps1`, elevated), commit `feat(pan): keyboard panning in the tick loop (#287)`.

### Task 6: Settings UI
**Files:** Modify `ui/src/settings-schema.js` (four keybind rows after Zoom out: `{ key:'__panLeft', type:'keybind', label:'Pan left', vkKey:'panLeftVk', modsKey:'panLeftMods' }` etc., section desc mentions "while zoomed"; `panSpeed` slider after Zoom-out speed, 0.25-4 step 0.05 def 1.0 unit 'times', desc "How fast holding a pan key moves the view."), `ui/src/Settings.svelte` (`kbDefaults` pan entries 37/3 ...), `ui/src/lib/keybindRules.js` (`KEY_SLOTS` pan rows), `ui/tests/settings.spec.js`.
- [ ] Tests: rows read `Ctrl+Alt+Left/Right/Up/Down` by default; capture Ctrl+Alt+PageUp on Pan up writes `panUpVk=33`, `panUpMods=3`; a bare letter is refused; slider writes `panSpeed`.
- [ ] `npx playwright test`, commit `feat(ui): pan keybinds and pan speed (#287)`.

### Task 7: Verify and ship
- [ ] `build.bat test` and `npx playwright test` green; `build.bat` + `build.bat config` clean.
- [ ] Deploy the signed build. When the PC has been idle 60 s: scratchpad script (SendInput, `trackLog=1` temporarily) checks: at 1x Ctrl+Alt+Left reaches a test window; zoomed to ~4x it does not, and the log shows `view mouse -> keys`; a tap moves the view centre by ~screen/8/level; a 1 s hold moves further; a mouse move logs the pointer placed in the view. Restore the ini afterwards.
- [ ] Docs: README controls line, CLAUDE.md one line under INPUT SWALLOWING, `docs/architecture/06-input.md` and `07-cursor.md` short sections. Version 0.15.0.
- [ ] Push, open PR closing #287, ask the owner "merge?".
