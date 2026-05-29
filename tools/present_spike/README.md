# Present spike (issue #69)

Decide by measurement whether a DirectComposition flip-model overlay beats blt+DwmFlush without
breaking cross-process click-through or regressing background-app responsiveness. See the spec:
`docs/superpowers/specs/2026-05-29-dcomp-present-spike-design.md`.

## Build

    build.bat spike

Produces `clickprobe.exe` and `harness.exe` in the repo root.

## Run (clean desktop, then under the real workload)

Start the click target (a green window near the top-left), then run the harness per config.
Results append to `%TEMP%\present_spike_results.log`.

    start "" clickprobe.exe --seconds 120

    rem click-through (PASS/FAIL)
    harness.exe blt clickthrough
    harness.exe dcomp-nolayer clickthrough
    harness.exe dcomp-layered clickthrough

    rem pacing (avg/max dt, hitch counts)
    harness.exe blt pacing
    harness.exe dcomp-nolayer pacing
    harness.exe dcomp-layered pacing

    rem latency (median/p95 injection-to-receipt)
    harness.exe none latency-baseline
    harness.exe blt latency
    harness.exe dcomp-nolayer latency
    harness.exe dcomp-layered latency

    type "%TEMP%\present_spike_results.log"

## Real-workload run (decides it)

Run the **pacing** and **latency** matrices again with a game running fullscreen-windowed /
borderless on the target monitor, 4K HDR enabled. The background-game responsiveness is the metric
that got the prior attempt (#24) reverted, so its `latency` numbers (overlay present vs baseline)
are the deciding data. Paste the full results log back.

## Decision gate (from the spec)

1. Click-through MUST pass, or the config is dead.
2. A dcomp config must show lower `maxMs` / fewer hitches than `blt` (else no upside).
3. dcomp `medianMs` within ~1 frame (~7 ms @144Hz) of `blt`, and not worse than baseline by more.
4. Subjective confirmation under a real game.

If a dcomp config clears all four, proceed to the Phase 2 engine-migration plan (resurrect
`d93408a`'s `present=dcomp` into `render_engine`). If none does, record the negative result in
CLAUDE.md and close #69.

## Dev-machine results (static desktop, 2026-05-29, 144Hz, no game)

These are the automatable runs. They are indicative only; the static desktop does not reproduce
the blt/DWM microstutter (which needs a real composition load), so the real-workload run above is
the actual decider.

    clickthrough mode=blt           result=PASS (probe DOWNs after click=1)
    clickthrough mode=dcomp-nolayer result=FAIL (probe DOWNs after click=0)
    clickthrough mode=dcomp-layered result=PASS (probe DOWNs after click=1)

    pacing mode=blt           hz=144 frames=576 fps=144.0 avgMs=6.95 maxMs=7.92 hitch1.5x=0 big2.5x=0
    pacing mode=dcomp-nolayer hz=144 frames=576 fps=144.0 avgMs=6.95 maxMs=7.10 hitch1.5x=0 big2.5x=0
    pacing mode=dcomp-layered hz=144 frames=576 fps=144.0 avgMs=6.95 maxMs=7.07 hitch1.5x=0 big2.5x=0

    latency mode=none          overlay=0 samples=30 medianMs=16.00 p95Ms=16.78
    latency mode=blt           overlay=1 samples=30 medianMs=15.77 p95Ms=17.13
    latency mode=dcomp-nolayer overlay=1 samples=0  medianMs=-1.00 p95Ms=-1.00
    latency mode=dcomp-layered overlay=1 samples=29 medianMs=15.90 p95Ms=16.50

Reading: `dcomp-nolayer` eats clicks (the Windows constraint; the no-layer path is out).
`dcomp-layered` keeps cross-process click-through, holds 144 fps with a marginally tighter `maxMs`
than blt, and adds no latency over baseline on a static desktop. It is the only viable DComp
candidate; whether it actually beats blt rides on the real-workload run.
