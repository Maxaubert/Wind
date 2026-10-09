#pragma once
// Transform WARM-KEEPING - PURE, no <windows.h>, so it is unit-testable.
//
// THE SYMPTOM (field report 2026-08-26, harness tools/pan_wake_probe.ps1). While zoomed with the
// transform model, panning is smooth - but the FIRST movement after the hand pauses hitches, and a
// side-to-side pan hitches at each end, where the hand reverses through zero velocity. Native
// Windows Magnifier does not do this. Reproducible on demand with the harness, and confirmed by
// eye on the same runs the harness scored.
//
// MEASURED, over DOOM at 7x with an identical injected hand (DWM composition intervals):
//
//   config                          idle median   idle stalls/s   wake stalls/s
//   Wind, txWarmMode=0                 13.17ms         58.9            29.1
//   native Windows Magnifier            6.94ms          0.0             0.0
//   Wind, txWarmMode=1 (1px jitter)     6.94ms          0.2             0.0
//
// WHAT DOES NOT WAKE DWM (measured, so do not re-try without new evidence; these were warm modes
// 2-4 and are gone):
//   - Re-sending the SAME transform (old mode 2):    wake 23.8/s. Identical writes are ignored.
//   - Republishing the input transform (old mode 3): wake 26.5/s - WORSE than baseline.
//   - Perturbing the LEVEL by ~2e-5 (old mode 4):    looked perfect on every composition-rate
//     metric, but the first genuine source change still paid full price (~25ms in a per-tick
//     trace); only a real change to the sampled REGION keeps DWM's re-render path warm.
//   - An unrelated per-frame damage window:          wake 28.2/s, composition rate unchanged.
//
// The 1px translation jitter is the one that works, at a known cost: the view sits 1px off the
// truth for one tick per pulse (smooth sampling can render that as shaking; nearest masks it).
// Warm pulses only run where Wind writes the view itself (locked mouselook, Inspect, a caret or
// focus tracked view); DWM's own centring needs none.
//
// docs/HITCH-FINDINGS.md carries the full write-up and the measured dead ends.
namespace wind {

enum class TxWarm {
    None = 0,
    Jitter1px,        // shift the whole image 1px for one tick (mode 1)
};

// CADENCE (issue #246). Every warm write is a REAL source-rect change, so DWM re-renders the
// whole magnified screen for it - the full pan cost, at rest. Measured on the controlled solid
// target (tools/gpu_ab.ps1, 2026-09-03, 4K, RTX 5090): a zoomed session sitting STILL cost
// dwm.exe 16.1% GPU with per-tick warming and 0.0% with it off; native Magnifier at rest is 0.2%.
// That is the entire GPU gap the field reported - panning costs both magnifiers the same order
// (Wind 16%, native 12%). So the warm write is now a PULSE on a cadence: one 1px displacement
// and its return, then nothing until the next period. warmHz 0 = every tick (the pre-#246
// behaviour); the return half of an open pulse is never gated, so the view is off by 1px for
// exactly one tick per period, never longer.
struct TxWarmIn {
    bool   wroteThisTick      = false;  // a real, changed write already went out - nothing to warm
    bool   ramping            = false;  // the level is moving on its own; it is already waking DWM
    int    mode               = 1;      // 0 = off, anything else = the 1px pulse
    bool   allowed            = true;   // RunTick's ex.warmAllowed: false for a free pointer's view
    double applyLevel         = 1.0;
    int    warmHz             = 0;      // pulses per second; 0 = every tick
    bool   pulseOpen          = false;  // the displacing half went out; the return half is owed
    unsigned long long sinceLastWarmMs = 0;   // since the last pulse CLOSED (or the last real write)
};

inline TxWarm WarmAction(const TxWarmIn& in) {
    if (in.mode <= 0) return TxWarm::None;
    // Not for a free pointer's detached view (field video 2026-10-08: following a Discord caret,
    // the pulses showed as a 1 px shake). An open pulse still closes, so a view is never left
    // displaced when the owner changes mid-pulse.
    if (!in.allowed && !in.pulseOpen) return TxWarm::None;
    // A real write this tick already did the job, and a ramp writes a new level every tick anyway.
    if (in.wroteThisTick || in.ramping) return TxWarm::None;
    // Never warm at rest level. Below this the session is not magnifying, and poking DWM there is
    // the startup tax that issue #148 removed.
    if (in.applyLevel <= 1.001) return TxWarm::None;
    // An OPEN pulse always closes, before the cadence gate can say otherwise: a displaced view
    // must never be stranded by the period.
    if (!in.pulseOpen) {
        // Cadence: a new pulse only when the period has elapsed.
        if (in.warmHz > 0 && in.sinceLastWarmMs < 1000ull / (unsigned long long)in.warmHz) return TxWarm::None;
    }
    return TxWarm::Jitter1px;
}

}  // namespace wind
