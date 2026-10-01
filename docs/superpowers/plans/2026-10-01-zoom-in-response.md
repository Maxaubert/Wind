# Zoom-in response (issue #310): spec + plan

## Goal
Make the zoom feel immediate and smooth: measure the real response (press to first magnified
frame on screen) and the zoom-in/out stutter, and fix what earlier reviews already pinned down.
Owner request 2026-10-01. Build and review now; ALL measuring waits for the owner's go-ahead (the
screen is off overnight), so every behaviour change is behind an ini knob the harness can flip.

## Known causes (reviews 2026-09-30, docs/HITCH-FINDINGS.md)
1. **Slow enter tick (transform):** 14-16 ms median, tail to 50 ms. `TransformModel::setActive(true)`
   shows the cursor sprite and then BLOCKS on two `DwmFlush()` calls (~14 ms) before blanking the
   system cursor, so the hand-off never blinks (#221). The zoom's first frame waits behind it.
2. **Caret jump at zoom-in (~3% of zoom-ins):** enabling the caret/focus tracker publishes the
   current caret; within 1 s of a key press that reads as a new keyboard-driven caret event, so the
   view lurches to the caret and the pointer is then warped back (`view mouse -> caret` at the
   same millisecond as `transform session`).
3. **DWM's zoom-in spike in games (~35-42 ms, once per zoom):** DWM builds its magnification
   machinery when a session starts; the context is released 1.2 s after a zoom (`txIdleReleaseMs`)
   because a live context taxes every cursor change in games (#148). A longer linger trades one
   for the other; only measurement can pick the value.

## Design
- **A. Zoom timeline (`zoomTrace`, ini, default 0).** One log line per zoom-in and per zoom-out:
  - in: press -> wake/tick start (the hook stamps QPC on every edge it wakes the loop for),
    enter-tick total, inside it `setActive` (bridge + ensureMag) and the first present, the time
    from press to the first DWM composite after the enter tick (the loop's post-tick DwmFlush
    return), and the durations of the next 6 ticks.
  - out: teardown tick total and `setActive(false)` (identity park) time.
  Cheap QPC reads only; nothing logged when off.
- **B. Non-blocking cursor bridge (`txEnterBridge`, ini, default 1).** 0 = today's synchronous
  DwmFlush pair. 1 = show the sprite at the pointer as today, but instead of blocking, keep the
  system cursor visible for the next 2 ticks (each zoomed transform tick is one composite: the loop
  is DwmFlush-paced) and blank it on the third; present()'s hide branch waits for the same count.
  Same overlap the pair guaranteed, without stalling the enter tick.
- **C. Tracker settle at zoom-in (always on).** When the view-owner logic becomes enabled, events
  for the next 150 ms only re-baseline `lastSeq` (they are the tracker's activation publish, not
  typing). Mouse takeover and keyboard panning are unaffected. Pure, unit-tested.
- **D. A/B harness (runs only on the owner's go-ahead).** `tools/zoom_response_ab.ps1`: N zoom
  cycles (side buttons, 1.5 s apart) per configuration, on the desktop and in a game in FOCUS
  (the owner launches it), with PresentMon on `dwm.exe` (desktop) or the game, `zoomTrace=1`.
  Configurations: `txEnterBridge` 0/1 x `txIdleReleaseMs` 1200/15000. Reports per config:
  press->first-composite median/p90/max, enter-tick duration, frame spikes (> 2x median frame
  time) within 200 ms of each zoom-in and zoom-out, and re-runs any config where an outlier
  appears twice. Restores the ini afterwards; dims the screen between runs (colorDimPct 1, off
  during runs) per the owner's request.

## Out of scope
Changing defaults beyond B/C before data; render-engine reveal gating; the colour filter.

## Plan
- [ ] T1 `view_target.h`: `ViewOwnerState::{wasEnabled, settleMs}`, `kEnableSettleMs = 150`;
  tests in `tests/test_view_target.cpp` (activation publish within 150 ms never takes the view;
  after it, a key-driven caret event does; mouse/pan still work inside the window).
- [ ] T2 config: `zoomTrace` (0/1), `txEnterBridge` (0/1, default 1); parse + template + test.
- [ ] T3 `transform_model`: deferred bridge (`bridgeTicks_`), present() hide branch gated on it;
  `setActive(true)` reports its own split (bridgeMs, ensureMagMs) via a small struct getter.
- [ ] T4 `input_router`: `lastEdgeQpc()` stamped in `WakeMain`. `main.cpp`: the zoom timeline
  (enter-tick stamps, post-tick DwmFlush return, next 6 tick durations, zoom-out teardown); one
  Info log line each, only when `zoomTrace`.
- [ ] T5 harness script + README note; build; unit tests; deploy (no input injection, no
  measurement until the owner says go).
- [ ] T6 code review workflow (reviewer + adversarial verify), fixes, docs (HITCH-FINDINGS,
  architecture 05-transform-engine), version 0.15.4. Report "ready to test".

## Review Focus
1. Deferred bridge: the real cursor and the sprite both visible for 2 frames must stay aligned
   (at ~1.0x the transform is identity); a zoom-out or engine switch before the counter elapses
   must not leave the cursor blanked or the counter stuck.
2. Inspect, game-inspect and mouselook (app-hidden cursor) paths: the bridge must not fight them.
3. The settle window must not swallow a real caret move typed right after zooming (150 ms).
4. zoomTrace must cost nothing when off and must not allocate or log inside the hook.
5. The harness must never leave the user's ini changed or the screen undimmed afterwards.

## Amendments after the plan review (2026-10-01)
- **C rewritten.** The caret jump is a stale `lastSeq`, not the tracker's activation publish (the
  tracker already baselines its first caret after activation, and a same-millisecond jump cannot
  come from its 30 ms coalesce). `StepViewOwner` runs only while zoomed, so an event published just
  before a zoom-out read as new typing at the next zoom-in. Fix: re-baseline `lastSeq` on the
  rising edge of tracking-active (zoom-in, or tracking turning back on mid-zoom), nothing swallowed
  afterwards; tracking events are ignored while tracking is off.
- **B deferred until measured.** The deferred bridge would show two misaligned cursors during the
  first ramp frames (the sprite is magnified at the lens point, the welded real pointer is not), and
  would move the 14-cursor blank into the live context (#189). Whether the DwmFlush pair is what the
  user feels is unmeasured; the timeline below decides it. B only ever addressed the enter-tick
  median, not the 50 ms tail or DWM's 35-42 ms machinery build.
- **Timeline stamps.** The press is stamped only on zoom-in rising edges (first since taken, stale
  after 500 ms); the first composite after the enter tick comes from `DwmGetCompositionTimingInfo`
  (cFrame/qpcCompose), not the loop's DwmFlush (absent on the enter tick). `setActive(true)` reports
  bridge / ensureMag / warm.
- **Harness measures what the user sees.** Screen BitBlt sampling of a textured off-centre region:
  press -> first visible change, and ramp stall frames (a still frame inside the first 300 ms of the
  ramp). Plus PresentMon (dwm.exe or the game), the zoomtrace lines, caret jumps (trackLog). ABAB
  blocks; one discarded cycle after each ini flip; 3 s after zoom-out so the cold config is cold;
  still and moving pointer. Old (main) vs new build at test time for the optical metrics.
