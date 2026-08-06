# Stranded bind keys: end the zoom crawl (issue #167 follow-up)

Date: 2026-08-06
Status: proposed

## The field problem

Several times, most recently GTA V 2026-08-06 06:48-06:54 (log-captured), the zoom crawls on
its own: the level creeps up or down at a slow steady rate. Pressing the zoom keys only adds
to or subtracts from the crawl; in RDR2 a fresh press has sometimes cleared it, in GTA V it
does not. The 7-minute capture shows ~4,600 held-state edges: the zoom-out bind pulsing
DOWN for ~8 ms every ~250 ms, with the keyboard-hook watchdog logging an "evicted ->
re-installed" recovery at the same cadence.

## Root cause (log + code trace)

The Keychron Link receiver occasionally strands a key in Windows' async state: the DOWN
arrives while Wind's LL keyboard hook is dead (a game-launch load spike is exactly when
Windows evicts it), the matching UP is lost by the receiver, and no future event ever
clears it. GetAsyncKeyState then reports the key held indefinitely.

Wind's watchdog tell (issue #156) is: async says a bound key is down AND the hook says it
is up, sustained 250 ms => the hook must be evicted => reinstall. A stranded async key
satisfies that tell FOREVER, so the watchdog reinstalls every 250 ms. During each
reinstall's ~8 ms window `kbHookActive()` is false, `keyDown` falls back to polling, the
polling read sees the stranded key "held", and the zoom advances one tick's worth from the
ease-in floor. 8 ms of zoom, 4 times a second: the crawl. The watchdog is not healing
anything - the hook was alive all along - it is rhythmically exposing the phantom.

External fixes cannot see this: while the hook is alive it swallows real bind events, so
GetAsyncKeyState (which StuckKeyWatch reads) never reflects live bind activity, and the
stranded bit itself is only visible in the 8 ms gaps. The fix belongs in Wind, which owns
both signals.

## Design: three mechanisms, one file each

### 1. Stranded-key escalation in the watchdog (the cure)

Track, per watched vk, whether the divergence SURVIVED a successful reinstall. If the same
vk is still divergent (async down, hook up) after a reinstall completed and the hook is
alive again, an eviction cannot explain it - a live hook would have seen the key's events.
Classify it as a stranded async key and CLEAR it: inject that key's UP via SendInput
(scan code derived with MapVirtualKey; extended flag preserved), log
`stranded bind key vk=0x.. cleared (receiver phantom, not an eviction)`, and reset the
divergence dwell. The injected UP flips the async bit; the divergence ends; the loop stops.

Safety: the injected UP can at worst release a key the user genuinely holds across a
reinstall cycle - a key that was, by construction, already dead to Wind (hook alive, but
its down predates the hook, so `keyPressed` is false and zoom ignores it). Today that state
crawls; after the fix it reads "release and press again". The UP is injected once per
classification, not per tick, so it cannot spam.

### 2. Reinstall rate-limit (the backstop)

Reinstall requests are capped: at most one per 2 s, and after 3 reinstalls inside 30 s a
30 s cooldown. A real eviction still heals within 2 s worst-case (polling keeps binds
working meanwhile - that is the existing fallback design), but no misclassification can
ever pulse at 4 Hz again. Pure window logic (`ReinstallGate`, src/reinstall_gate.h) so the
cadence rules are unit-tested.

### 3. Zoom bind debounce (defense in depth)

Keyboard zoom binds must read held continuously for `bindDebounceMs` (ini, default 25,
0 = off, hot-reloadable) before the zoom controller sees them. The 8 ms exposure pulses can
never move the zoom regardless of what produces them; the shortest real press on record is
40 ms, and a 25 ms onset delay is under two frames at 144 Hz. Mouse side-buttons are not
debounced (no phantom history there, and #113 already nets them). Pure helper
(`BindDebounce`, src/bind_debounce.h), unit-tested with the captured 8 ms / 250 ms pulse
train and with real press shapes.

## What this does NOT do

- No change to hook installation, swallowing, or the eviction tell for FIRST detection -
  a real eviction still recovers exactly as fast as today (one reinstall, immediately).
- No handling for the hook-suspended (noSwallowApps) polling mode: no captured incident
  runs in it. If one appears, the raw-input stream (#168) enables the same
  stranded-vs-real discrimination there; deliberately deferred.
- StuckKeyWatch stays as-is (its modifier scope is correct and unaffected).

## Testing

Unit (pure): BindDebounce truth table (pulse train from the capture never passes; 40 ms
press passes at 25 ms boundary; 0 disables), ReinstallGate windowing (cadence caps,
cooldown entry/exit), config parse of `bindDebounceMs`.

Field: deploy; the crawl signature (4 Hz held-edge pulse train + recovery storm) must not
recur in the next GTA V / RDR2 sessions; a `stranded bind key` log line appearing instead
is the fix working. Real eviction recovery verified by launching a heavy game and checking
binds still heal (existing behaviour).

## Issue / PR mapping

One issue (#176), one branch (`fix/176-stranded-bind-keys`), one PR.
