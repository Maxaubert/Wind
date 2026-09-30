# Scroll-wheel zoom and safe keybinds: implementation plan

**Spec:** `docs/superpowers/specs/2026-09-30-wheel-zoom-and-safe-keybinds-design.md`.
Branch `feat/285-wheel-zoom` (worktree `Wind-wheel`).

## Global constraints
- No em-dashes. Pure headers do not include `<windows.h>`.
- The hook stays cheap: no allocation or blocking work in the mouse/keyboard hook callbacks.
- Swallows stay balanced (only swallow an up whose down was swallowed); wheel notches have no up.

## Tasks
1. **Rules** - `src/keybind_rules.h` (`CheckKeyBind`, `CheckWheelBind`, `CheckClickBind`, reason codes) and
   `tests/fixtures/keybind_cases.txt` + `tests/test_keybind_rules.cpp`. `ParseConfig` sanitises every
   bind with them (replaces the bare `IsForbiddenBindVk` sanitising; the hook keeps its own
   never-swallow check). Commit `feat(keybinds): one safety rule set for every bind (#285)`.
2. **Mask keystroke** - input router: when a swallowed key-down belongs to a combo with Alt or Win,
   inject VK 0xE8 down/up once. Test the decision as a pure function. Commit.
3. **Clicks** - config `...ButtonMods` per zoom slot, button values 3-5; the mouse hook swallows a
   matching click down/up as a balanced pair and reports it held; RunTick's inHeld/outHeld include it.
   Pure matcher tested (mods subset, balanced up). Commit.
4. **Wheel** - config `zoomWheelMods`, `zoomWheelStepPct`; `WheelAccum` (pure: 120-unit accumulation)
   and `ZoomController::stepTarget(n, step)` + target glide in `tick` (pure, tested); mouse hook swallows
   matching notches and queues whole steps to the tick; RunTick feeds them to the controller. Commit.
5. **UI** - `ui/src/lib/keybindRules.js` (+ tests against the shared case list), `KeybindCapture`
   refuses with a reason (visible + live region), a wheel-capture row and a step slider in the Keybinds
   section. Playwright tests per spec section 7. Commit.
6. **Verify + ship** - unit + UI tests, build, deploy, owner's-PC checks per spec section 7 (SendInput),
   docs (CLAUDE.md input-swallowing gotcha: the new rule set and the mask keystroke; README feature
   line), version 0.12.x minor bump, review workflow, PR, owner says merge.
