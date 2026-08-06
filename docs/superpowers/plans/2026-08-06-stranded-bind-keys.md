# Plan: stranded bind keys (spec 2026-08-06-stranded-bind-key-design.md)

Branch `fix/176-stranded-bind-keys` from main. Tasks in order; build + full test suite after
each code task; deploy once at the end.

## Task 1: pure helpers + tests

- `src/bind_debounce.h`: `class BindDebounce { bool update(bool rawHeld, unsigned long long nowMs, int debounceMs); }`
  - returns the debounced held state: true only once rawHeld has been continuously true for
    >= debounceMs; debounceMs <= 0 passes rawHeld through unchanged. Pure, no windows.h.
- `src/reinstall_gate.h`: `class ReinstallGate { bool allow(unsigned long long nowMs); }`
  - encodes: min 2000 ms between allowed reinstalls; >3 allowed inside 30 s => deny until
    30 s after the last allowed one. Pure.
- `tests/test_bind_debounce.cpp`: pulse train 8 ms on / 242 ms off never passes at 25 ms;
  continuous hold passes at exactly 25 ms; 0 disables; release resets the clock.
- `tests/test_reinstall_gate.cpp`: burst of requests at 250 ms cadence lets 1 through per
  2 s, hits the cooldown after 3, recovers after the cooldown.
- `tests/test_config.cpp`: `bindDebounceMs` parses, defaults to 25, clamps at 0.

## Task 2: config knob

- `src/config.h/.cpp`: `int bindDebounceMs = 25;` parse + hot-reload (existing pattern).

## Task 3: wire the debounce

- `main.cpp` keyDown/held-state block: run the four keyboard zoom vks' combined held
  through two `BindDebounce` instances (in-channel, out-channel) with `cfg.bindDebounceMs`.
  Mouse side-buttons bypass. Recenter/inspect/swap binds unchanged (edge-triggered taps).

## Task 4: watchdog escalation + rate-limit

- `input_router`: expose `unsigned kbHookReinstalls()` (exists) + a way to know a reinstall
  COMPLETED since a given point (reinstall counter snapshot is sufficient).
- `main.cpp` watchdog block:
  - keep first-detection behaviour, but route `requestKbHookReinstall()` through a
    `ReinstallGate` member on TickState.
  - remember the divergent vk set + the reinstall counter at request time; when the
    counter has advanced (reinstall completed, hook active) and the SAME vk is divergent
    again for the dwell, inject that vk's key-up (SendInput, scan from MapVirtualKey with
    extended flag), log `stranded bind key vk=0x%X cleared (receiver phantom, not an
    eviction)`, reset dwell. One injection per classification.
- The injected UP uses the existing forbidden-vk guard implicitly (only configured bind
  vks are watched, and IsForbiddenBindVk already keeps catastrophic keys out of binds).

## Task 5: docs

- `CLAUDE.md`: watchdog section gains the stranded-key escalation + rate-limit (tell is
  ambiguous: eviction vs stranded async key; surviving a reinstall disambiguates).
- `docs/KNOWN-ISSUES.md`: #167 entry updated - crawl root cause (watchdog exposure loop),
  fixed by #176; RDR2-re-press folklore explained (it cleared only when the press landed in
  an exposure gap).
- Memory (`project_stuckkeywatch.md`): crawl mechanism corrected - NOT typematic repeats;
  watchdog exposure loop; StuckKeyWatch extension dropped as structurally blind here.

## Task 6: verify + ship

- `build.bat test` green, full build green.
- PR referencing #176; deploy via uiaccess_setup.ps1; restart Wind; tell Max what to watch
  for (no crawl; `stranded bind key` log line = fix firing; binds still heal after heavy
  game launch).
