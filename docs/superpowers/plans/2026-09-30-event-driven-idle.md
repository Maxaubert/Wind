# Event-driven idle loop (issue #71): spec + plan

## Goal
At 1x with nothing happening, Wind's main loop sleeps instead of ticking at the monitor refresh
(144 Hz here, ~6% of one core). It wakes at once on anything that can start a zoom, and on a slow
housekeeping timeout for the checks that have no event. Zoom-in latency must not get worse.
Owner request 2026-09-30 ("should the loop always run ... what's the benefit"). The zoom-in/out
frame spike is a separate issue and out of scope.

## Findings that shape the design (inventory 2026-09-30)
- The idle wait is `SetWaitableTimer` + `WaitForSingleObject(timer)` at `pacedHz`, re-armed every
  iteration (src/main.cpp main loop, the `!renderPresentPaces && !dwmPaces && !pacedByPulse` branch).
- Zoom triggers reach the main thread only as atomics set on the hook thread (MouseProc/KbProc) or
  as WM_HOTKEY (quick zoom, hide cursor). Nothing wakes a sleeping loop today.
- WM_INPUT (Raw Input, RIDEV_INPUTSINK) arrives for EVERY mouse move system-wide, so the wait must
  NOT include QS_RAWINPUT (QS_ALLINPUT/QS_INPUT would wake at the mouse polling rate).
- Housekeeping that rides the tick with its own wall-clock gates: config watch (250 ms), cover
  probe / launch quiesce (250 ms), noSwallowApps probe (100 ms), cursor-tint fullscreen probe
  (250 ms), HDR re-read (1 s), keyboard-hook eviction watchdog (250 ms dwell), transform idle
  context release (deadline ~1.2 s). All tolerate ~100 ms granularity.
- The config gate accumulates tick `dt`; the zoom controller clamps `dt` to 50 ms. After a sleep the
  first tick's `dt` is the whole sleep: fine for the config gate, but it would make the first zoom
  step up to 50 ms of ramp (a visible jump) and log idle gaps as hitches in the tray pacing readout.
- The caret/focus tracker is switched off only from the zoomed view block; a zoom-out that snaps
  straight to 1.0 can leave it ACTIVE at 1x, where its 16 ms backstop timer resolves the caret via
  UIA ~60 times a second. Its thread also keeps the 16 ms timer running while inactive.

## Design
1. **Idle mode** (`TickState::idleSleepOk`, computed at the end of RunTick): true only when not
   active and not Inspect, no zoom direction or pan key held, no wheel steps pending, no quick-zoom
   request pending, `restAfterReveal == nullptr`, `revealPending == 0`, the last active tick was
   >= 500 ms ago (teardown, rests and the tint re-apply run at full rate first), the mouse hook is
   installed, and EITHER the keyboard hook is active OR no keyboard bind is configured (with the hook
   gone, keyboard binds are polled with GetAsyncKeyState and must keep the refresh-rate tick).
   The magnify model counts as idle between presses (its holds keep the loop awake).
2. **Wake event**: an auto-reset `g_idleWake` event owned by InputRouter (`wakeEvent()` getter).
   The hooks `SetEvent` it only on edges that matter, never on plain moves or unbound keys:
   bound-key down/up (inside the existing bound-key block of KbProc), a matched button/click bind
   down and its up (MouseProc, XBUTTON path via `setButtonState`), and a wheel step added. SetEvent
   runs after the atomics are published.
3. **Idle wait**: when `idleSleepOk`, the loop replaces the timer wait with
   `MsgWaitForMultipleObjectsEx(1, &wake, 100, QS_POSTMESSAGE | QS_SENDMESSAGE | QS_HOTKEY | QS_TIMER, 0)`.
   100 ms keeps every housekeeping gate within its budget. The quit event and config watch stay
   polled by RunTick (not in the wait set: the quit event is auto-reset and the config handle is
   re-armed only inside RunTick's gate, so either would be consumed or busy-spin). Messages keep
   being drained at the top of every iteration as today (WM_INPUT included, so the raw-UP safety
   net still runs, just on the next wake).
4. **First tick after a sleep** (`TickState::wokeFromIdle`): RunTick keeps the full `dt` for the
   housekeeping gates but clamps the MOTION `dt` to one refresh interval, and skips the tray
   `ticks.push` and the diagnostics hitch count for that tick.
5. **Tracker**: `g_track.setActive(false, ...)` in the active->idle teardown branch; the tracker
   thread runs its backstop timer at 16 ms while active and 250 ms while inactive (the shell-panel
   re-check keeps working, only slower, and matters only when zoomed).

## Out of scope
The zoom-in/out frame spike; the render/transform paced paths; the device-lost branch; txPace=2.

## Plan
- [ ] T1 `InputRouter`: `g_idleWake` (CreateEventW auto-reset in the constructor or start(), closed
  in stop()), `HANDLE wakeEvent() const`, a static `WakeMain()` helper; SetEvent at the edges in
  item 2. No logging, no allocation in the hooks.
- [ ] T2 `TickState`: `bool idleSleepOk = false, wokeFromIdle = false; unsigned long long lastActiveMs`.
  End of RunTick: compute `idleSleepOk` per item 1 (reuse `inHeld/outHeld` and the pan held state
  already computed in the tick; `lastActiveMs` stamped whenever `active`).
- [ ] T3 main loop: when `ts.idleSleepOk` and the timer branch would run, do the MsgWait instead and
  set `ts.wokeFromIdle = true`; else unchanged.
- [ ] T4 RunTick prelude: when `wokeFromIdle`, skip `ticks.push` and clamp the motion dt: introduce
  `rawDt` for `sinceCheck` and diagnostics, `dt = min(rawDt, 1.0 / max(hz, 30))` for everything
  else; clear the flag.
- [ ] T5 tracker: setActive(false) at the active->idle edge; tracker timer 16 ms active / 250 ms idle.
- [ ] T6 verify: unit tests (the pure idle predicate in a new `src/idle_policy.h` with doctest
  cases for every condition in item 1); build; deploy; measure Wind CPU at 1x for 10 s before/after
  (target: well under 1% of one core); live SendInput check that side-button, keyboard, wheel and
  click zooms still start, quick-zoom hotkey still toggles, and a config edit still hot-reloads
  within ~0.5 s; zoom-in latency sanity (first level change after the press within one frame).
- [ ] T7 docs (CLAUDE.md one line, docs/architecture/02-tick-loop.md section), version 0.15.3,
  code review workflow, PR, deploy the branch build, recommend, ask the owner "merge?" (no standing approval).

## Review Focus
1. A zoom key pressed while the loop sleeps: wakes within ~1 ms, and the first zoom step is one
   frame's worth, not a 50 ms jump.
2. Keyboard hook suspended (noSwallowApps app in front) or evicted with keyboard binds set: the loop
   must not sleep, or keyboard zoom would lag up to 100 ms.
3. A busy-spin: any wait object or message left signalled makes the wait return immediately forever
   (check the auto-reset event and that WM_INPUT is excluded from the wake mask).
4. Magnify model: holding the zoom key must keep ticking every frame (nativeZoomTick notches).
5. The tray menu path that calls RunTick from WM_TIMER must still work while the loop sleeps.

## Amendments after the plan review (2026-09-30)
- Messages are drained right after the wait returns (a hotkey was otherwise seen 100 ms late).
- The wake is raised in `PublishButtonHeld` (covers side buttons AND the left/right/middle click
  binds, press and release), on the first down / up of a bound key (no auto-repeat wakes) and per
  wheel step.
- The predicate is evaluated live in `IdleNow` right before the wait (the magnify model returned
  before the end of RunTick, and a cached flag could be stale).
- The wake event is created before the WIND_NOHOOK return and closed after the hook thread joins; any
  wait result other than the event, the quit event, a message or the timeout falls back to the timer.
- The quit event is in the wait set (instant quit). QS_TIMER dropped (Wind has no thread timers).
- The focus tracker installs its LOCATIONCHANGE hook and 16 ms poll only while active (250 ms panel
  re-check otherwise), switched by a dedicated thread message.
- Code review fixes: the wake tick skips the tray pacing ring and diagnostics, raw motion from before
  an activation is zeroed, and a bound key the OS reports held keeps the loop awake (evicted hook).
