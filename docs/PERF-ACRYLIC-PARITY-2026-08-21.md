# Wind vs native Magnifier over acrylic (issue #219) - iteration log, 2026-08-21

Max's report: Wind performs far worse than wm zoomed ~14-15x over the (maximized, acrylic)
Prism window; the hitch is MOSTLY at zoom-in, a bit while panning, and swapping focus to
another maximized window (Tabby) and back helps reproduce it.

Instrumentation (tools/mag_perf_run.ps1): identical injected zoom+pan cycles for both drivers;
DwmFlush inter-return intervals for compositor pacing (DwmGetCompositionTimingInfo is
unavailable on this VRR panel - 0x88980090 at every struct size); level plateau/jump evenness
and offset-change gaps via MagGetFullscreenTransform read-back on the Mag-affine thread; GPU
3D-engine counters per process in a child sampler; CPU/WS per process group. Cycle mode = the
focus-swap repro: activate Tabby, activate Prism, zoom to 15x, pan 2.5s, zoom out; 20 cycles.

## Findings

1. Steady-state pans, fast pans, and ramp cycling all measure IDENTICAL Wind vs wm (compositor
   143.6fps, no stutters, GPU ~5%, both drivers). The gap is not steady-state.
2. The 20-cycle soak found the real artefact, matching Max's description exactly:
   - UNCAPPED Wind: 3/20 zoom-ins freeze 35-43ms mid-ramp then SNAP 1.2-1.9 levels at once,
     with a matching compositor gap; 2/20 pans freeze 43-46ms. Sporadic, DWM-internal
     (txwrite stayed <5ms - not the write call; input-transform machinery off changed nothing:
     2/20 + 2/20 with magInputTransform=0).
   - NATIVE 20-cycle tail is WORSE: over-25ms ramp stalls in 7/20 cycles, routine 23-32ms
     gaps every ramp (7-14 coarse steps, jumps up to 3.9 levels), pan spikes 36-63ms in 3/20.
     Native's uniformly coarse ease masks its stalls; Wind's 143Hz fine cadence makes its rarer
     freeze-then-snap maximally visible. That perceptual asymmetry IS the reported "way worse".
3. THE FIX: txMaxStepPct=25 (cap applied level change at 2.5%/tick; knob existed, shipped 0).
   20-cycle soak with the cap: EVERY ramp even - plateau <=13ms, uniform 0.36-level steps,
   zero over-25ms compositor gaps, ramp ~35ms longer (890 -> 924ms). Pan clean (worst 18.5ms
   flush gap) once the catch-up tail is kept out of the pan window; the 8/20 pan stalls in the
   no-settle run were top-of-zoom catch-up level writes landing during the pan - in real use
   that is ~2 capped ticks right after release.
   Normal ramp ticks are 0.8-2.2% relative, so the cap only ever bites the post-stall snap.
4. Cursor guardrail with the cap: devMed 0px (welded), p95 270 (reversal transients, same as
   baseline), 143.6fps, zero stutters. No cursor regression.

## Outcome

txMaxStepPct default 0 -> 25 (config.h; hot-reloadable). Wind now measures BETTER than native
on every phase of Max's own repro: ramps 20/20 clean vs native 13/20; pan worst 18.5ms vs
native routine ~30ms with 63ms spikes.

Numbers that define "parity or better" for future regressions (15x, focus-swap cycle, acrylic):
ramp plateau <=13ms / jump <=0.4 levels / no over-25ms flush gaps; pan flush gaps <=20ms.

Prior config.h comments say txLevelStep and txGrid measured NO BETTER / WORSE - those skip or
quantize writes. The cap is different: it never skips, it limits the SIZE of a change, which is
what kills the snap without touching cadence.

## Round 2: session-start bounce (Max field report after the cap shipped)

Quick re-zoom (15x -> full out -> immediately in) made the level BOUNCE at ramp start: rig-
reproduced 4/4 with the harness rezoom mode (5-7 backward level steps, 0.13-0.19 levels of
backward travel). Cause: the cap's DOWN clamp anchored on a stale lastLevel_ - zoom-out trailed
under the cap and the identity park wrote 1.0 without updating the cache. Fix (ae1f126): cap
UP-steps only (zoom-out measured clean uncapped, outGaps <=8ms in every soak) + the park syncs
lastLevel_. After: back0 on every re-zoom, ramps unchanged. Residual: a sporadic DWM stall
(~1/10 ramps, system-internal) still occurs but now recovers as a capped glide, not a snap.

## Round 3: Max's zig-zag protocol (bottom -> top climb at 15x, both pan axes, 8 cycles each)

- WIND: 8/8 clean. Ramps even (back0), zig offset gaps max 21-29ms, compositor flush gaps
  max 18.9ms, ZERO over-25ms stutters in any phase. Cursor welded: dev median 0.0 both axes.
  Resources: dwm CPU ~0, GPU ~0.5%, dwm WS 157MB, Wind WS 19MB.
- NATIVE: stutters in EVERY zig pass - 1-6 over-25ms compositor stalls per climb (28 across
  8 cycles), ramp gaps 24-28.6ms routinely, plus Magnify.exe 124MB WS / 1.3% CPU.

Wind is as good or better than wm on every protocol measured: steady pan, fast pan, ramp
cycling, focus-swap cycles, quick re-zooms, and the zig-zag climb.
