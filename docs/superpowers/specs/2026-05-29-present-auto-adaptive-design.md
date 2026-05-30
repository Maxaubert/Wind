# present=auto: adaptive dcomp/blt present switching (issue #69)

Date: 2026-05-29
Issue: [#69](https://github.com/Maxaubert/Wind/issues/69)
Builds on: `docs/superpowers/specs/2026-05-29-dcomp-present-engine-design.md`
Status: design approved, pre-implementation

## Problem

The `present=dcomp` flip-model path is smoother than blt for the common cases (game in the
foreground, normal desktop apps) - confirmed by in-game testing. But in one specific state - a
**windowed app in the foreground over a still-running background fullscreen game** - DWM throttles
the on-screen composite of our DComp flip visual to ~60Hz (an MPO / multi-flip composition limit).
This was root-caused exhaustively: all three present-pacing options (`Present(1,0)`,
`Present(0,0)`+`DwmFlush`, timer+`Present(0,0)`) hit the same ~60 on-screen ceiling, while the
decoupled loop still ran 131fps/0-hitch - proving it is DWM's composite of the flip visual, not our
loop or pacing. blt's redirection surface is not a second flip swapchain, so DWM keeps it at full
refresh; blt measured a solid 140-144fps in the identical scenario and felt smooth.

Conclusion: neither present mode wins everywhere, and it cannot be fixed by pacing. The only way to
get the best of both is to run dcomp normally and fall back to blt in the bad state.

## Goal

Add `present=auto` (the new default): run dcomp when it is smooth, and automatically fall back to
blt when the bad-state throttle is detected, switching back to dcomp when the state clears. All
switches happen at a zoom boundary so they are hitch-free and never reset the zoom.

## Non-goals

- Not changing the dcomp or blt present paths themselves (done in the prior spec). This only adds
  the policy that chooses between them.
- Not attempting to defeat DWM's composite throttle (proven not possible from app code).
- Not mid-zoom switching (a swapchain rebuild mid-zoom would be a ~100ms freeze - explicitly out).

## Hard constraints (from the user)

- **Never reset the zoom** on a switch. (Zoom state lives in `ZoomController`/`CursorMapper` in
  `TickState`, entirely outside `RenderEngine`, so `setPresentMode` cannot affect it - this holds
  by construction.)
- **No noticeable hitch** on a switch. Guaranteed by only switching at a zoom boundary, while the
  overlay is hidden (the swapchain rebuild is invisible).
- Accepted residual: the first zoom into a freshly-entered bad state is laggy before the policy
  adapts, and a rare failed re-probe zoom. This floor is unavoidable (we can only detect the
  throttle by presenting into it).

## Architecture

The engine is unchanged. The new logic is split into a pure, testable policy plus thin Win32
wiring in the tick - matching the existing codebase split (pure `lock_detector` + Win32 in main).

### 1. Config (`src/config.h`, `src/config.cpp`)

- `present` accepts `"blt" | "dcomp" | "auto"`; **default `"auto"`** (was `"blt"`). Unknown -> `"auto"`.
- Update the serialized default block + comment to document `auto` and the three values.
- Update the unit test in `tests/test_config.cpp` (default is now `auto`; `auto`/`blt`/`dcomp`
  parse; unknown -> `auto`).

### 2. Pure policy (`src/present_policy.h`, `src/present_policy.cpp`)

No `<windows.h>`. Compiled into both the app and the `WIND_TESTS` build (added to `build.bat`'s
test source list), and doctest-covered like `lock_detector`.

Interface:

```cpp
namespace wind {
enum class PresentChoice { Blt, Dcomp };

class PresentPolicy {
public:
    PresentPolicy();
    // Advance the policy one tick and return the desired present mode.
    //  dt                 - seconds since last tick (real time, NOT zoom-clamped)
    //  zoomed             - is the magnifier currently zoomed (we only present/measure then)
    //  fps                - measured loop fps this tick (instantaneous or short-window; <=0 if unknown)
    //  refreshHz          - detected display refresh
    //  foregroundFullscreen - the foreground window covers the target monitor (game likely in front)
    //  foregroundChanged  - the foreground window handle changed since the last tick
    PresentChoice update(double dt, bool zoomed, double fps, int refreshHz,
                         bool foregroundFullscreen, bool foregroundChanged);
    PresentChoice choice() const;   // current desired mode
    void reset();                   // back to the optimistic initial state (Dcomp)
private:
    // ... state: choice_, belowThresholdSecs_, sinceProbeSecs_, justProbedSecs_, etc.
};
}
```

State machine (defaults in parentheses, all easy to tune):
- Initial `choice_ = Dcomp` (optimistic - the common case is smooth).
- **Dcomp -> Blt (throttle):** while `zoomed` and `choice_ == Dcomp`, accumulate time where
  `fps > 0 && fps < kThrottleFrac * refreshHz` (kThrottleFrac = 0.7). When that reaches
  `kThrottleConfirmSecs` (1.0s), set `choice_ = Blt`, reset the probe backstop. Recovery above the
  threshold resets the accumulator (so brief dips do not trip it).
- **Blt -> Dcomp (re-probe):** while `choice_ == Blt`, accumulate `sinceProbeSecs_`. Probe (set
  `choice_ = Dcomp`) when EITHER a clear cue fires - `foregroundFullscreen` becomes true, or
  `foregroundChanged` - OR `sinceProbeSecs_ >= kBackstopSecs` (60s). On probing, start a short
  `justProbedSecs_` watch.
- **Failed probe:** if, within `kProbeGraceSecs` (1.0s) of probing into Dcomp, the throttle
  condition trips again, return to Blt and reset the backstop (so we do not oscillate; the next
  probe waits for a fresh cue or the backstop again).
- `choice()` returns `choice_`. The caller decides when to *act* on it (zoom boundary).

The policy is pure: it does no I/O and never calls `setPresentMode`. It only maps signals -> a
desired mode. This makes the tricky hysteresis fully unit-testable.

### 3. Win32 wiring (`src/main.cpp`)

- `TickState` gains: `PresentPolicy presentPolicy`, `bool presentAuto` (cfg.present == "auto"),
  `PresentMode desiredPresent` (what we want the engine to be), `HWND lastForeground` (for change
  detection).
- `PresentModeFromCfg`: `"dcomp"` -> Dcomp, `"blt"` -> Blt, `"auto"` -> Dcomp (the auto policy's
  optimistic start; `presentAuto` is set separately).
- Generalize the existing zoom-boundary apply: today `presentChangePending` re-applies
  `PresentModeFromCfg(cfg)`. Replace it with a `desiredPresent` field that is applied at the zoom
  boundary (idle at 1x, or first thing on zoom-in) via `setPresentMode` when it differs from
  `renderEngine.presentMode()`. Both the ini-driven change and the policy set `desiredPresent`.
- Each tick in `RunTick`, when `presentAuto`:
  - Compute `fps` from the real `dt` (the loop already has it; `fps = dt > 0 ? 1.0/dt : 0`).
  - Read the foreground state cheaply: `HWND fg = GetForegroundWindow();`
    `foregroundChanged = (fg != t.lastForeground); t.lastForeground = fg;`
    `foregroundFullscreen =` fg's `GetWindowRect` covers the target monitor rect (within a small
    margin). This runs every tick, including at 1x (so a state-clear while idle is noticed and the
    next zoom-in comes up as dcomp).
  - `PresentChoice c = t.presentPolicy.update(dt, lvl > 1.0, fps, t.hz, foregroundFullscreen, foregroundChanged);`
    Map `c` -> `desiredPresent` (Blt/Dcomp).
  - The zoom-boundary apply (existing machinery) does the actual `setPresentMode` only when hidden.
- When `present` is `blt` or `dcomp` (not auto): `presentAuto = false`, `desiredPresent` is pinned
  to that mode, the policy is not consulted (and is `reset()` so a later switch to auto starts clean).
- On a hot-reload that changes `present`: recompute `presentAuto`/`desiredPresent`; mark the
  zoom-boundary apply pending (as today).

### 4. Diagnostics

When `diagnostics=1`, log each effective-mode switch with its reason (`throttle`, `cue`,
`backstop`, `failed-probe`, `ini`) to `wind_diag.log`, so the behavior can be verified without a
debugger.

## Data flow

`RunTick` -> gather (dt, zoomed, fps, refreshHz, foreground state) -> `PresentPolicy::update` ->
`desiredPresent` -> (at zoom boundary, overlay hidden) `RenderEngine::setPresentMode` -> engine
rebuilds the swapchain for the new mode. The capture/magnify/cursor pipeline and the zoom state are
untouched.

## Invariants preserved

- Hitch-free + zoom-preserving switches (zoom-boundary only; zoom state outside the engine).
- Everything from the dcomp spec still holds in whichever mode is active (WDA_EXCLUDEFROMCAPTURE,
  cross-process click-through, alt-tab no-flash, topmost re-assert, HDR).
- `present=blt` reproduces today's shipped behavior exactly.

## Error handling

- `setPresentMode` failure (rare): keeps the working pipeline and returns false (already
  implemented); the policy will simply try again at the next boundary.
- Detection false-positive (switch to blt when not actually throttled): harmless - blt is smooth
  everywhere; the re-probe path returns to dcomp when a cue fires.

## Testing / verification

- `tests/test_present_policy.cpp` (doctest): initial choice is Dcomp; sustained sub-threshold fps
  while zoomed flips to Blt after ~1s; a brief dip does NOT flip; foreground cue and the backstop
  each flip Blt->Dcomp; a failed probe returns to Blt and does not oscillate; `present=blt/dcomp`
  bypass is exercised at the wiring level.
- `build.bat test` green (policy added to the test sources).
- `build.bat check` clean.
- Self-tests still pass in `present=blt` and `present=dcomp`.
- In-game validation by the user: (a) game foreground -> stays dcomp, smooth; (b) windowed app over
  running game -> first zoom laggy, then auto-falls to blt and stays smooth; (c) bring the game back
  / close it -> returns to dcomp. Diag log shows the switches + reasons.

## Default tunables

`kThrottleFrac = 0.7`, `kThrottleConfirmSecs = 1.0`, `kBackstopSecs = 60.0`,
`kProbeGraceSecs = 1.0`. Constants in `present_policy.cpp`, easy to adjust after real-world feel.

## Follow-ups (out of scope here)

- Record the dcomp/auto findings + the windowed-over-game gotcha in CLAUDE.md.
- Once `present=auto` is validated in-game, advance/close #69; decide whether blt's separate
  `dwmFlush` microstutter workaround is still needed.
