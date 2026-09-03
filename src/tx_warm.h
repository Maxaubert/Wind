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
//   Wind, txWarmMode=4 (level eps)      6.94ms          0.0             0.0
//
// WHAT DOES NOT WAKE DWM (measured, so do not re-try without new evidence):
//   - Re-sending the SAME transform (mode 2):        wake 23.8/s. Identical writes are ignored.
//   - Republishing the input transform (mode 3):     wake 26.5/s - WORSE than baseline.
//   - An unrelated per-frame damage window:          wake 28.2/s, composition rate unchanged.
//   - A level change of 4e-6 relative:               too small to register at all.
//
// WHY MODE 4 SHIPS ON, and what is still unresolved.
//
// Field verdict (2026-08-26): with mode 4 the reporter had no stutter and saw no shimmer; with it
// off the stutter returns immediately. That is the decisive evidence, and it outranks the two
// concerns below - both of which are about the EXPLANATION, not about whether it works.
//
//   1. The displacement is bigger than first claimed. 0.077 is in SOURCE pixels, so on screen it
//      is 0.077 * level: ~0.6px at 7x, ~1.6px at 21x. Mode 1's rigid 1px shift is smaller above
//      ~13x, so if shimmer is ever reported at high zoom, try mode 1 there before anything else.
//      The applied stream also shows the derived source origin flipping a whole source pixel
//      (offX 2411 <-> 2412 at 7.37x) as the rounding tips back and forth.
//   2. The "native never goes quiet" story is WRONG. Sampling native's applied stream shows it
//      writes NOTHING across a 330ms rest - one level value for a whole run - and still holds
//      6.94ms composition. So native is not staying smooth by keeping warm, and we do not know
//      what it actually does. Mode 4 fixes the symptom; it does not explain native.
//   3. The harness metric is bimodal on one binary (0.00 vs 18-29 stalls/s, nothing changed).
//      VRR refresh hunting is the leading suspect - the panel runs 23-143Hz and composition
//      settles at either ~144Hz or the game's ~72Hz. Single takes are therefore not conclusive;
//      the user's eyes have been the tiebreaker throughout.
//
// docs/HITCH-FINDINGS.md carries the full write-up and the measured dead ends.
namespace wind {

enum class TxWarm {
    None = 0,
    Jitter1px,        // mode 1: legacy - shifts the whole image 1px. Kept for A/B only.
    SameValue,        // mode 2: measured dead
    InputTransform,   // mode 3: measured dead
    LevelEpsilon,     // mode 4: THE SHIPPED FIX
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
    int    mode               = 4;
    double applyLevel         = 1.0;
    int    maxLevel           = 0;      // 0 = no cap
    int    windowMs           = 0;      // 0 = warm for as long as the session rests
    unsigned long long sinceLastChangeMs = 0;
    int    warmHz             = 0;      // pulses per second; 0 = every tick
    bool   pulseOpen          = false;  // the displacing half went out; the return half is owed
    unsigned long long sinceLastWarmMs = 0;   // since the last pulse CLOSED (or the last real write)
};

inline TxWarm WarmAction(const TxWarmIn& in) {
    if (in.mode <= 0) return TxWarm::None;
    // A real write this tick already did the job, and a ramp writes a new level every tick anyway.
    if (in.wroteThisTick || in.ramping) return TxWarm::None;
    // Never warm at rest level. Below this the session is not magnifying, and poking DWM there is
    // the startup tax that issue #148 removed.
    if (in.applyLevel <= 1.001) return TxWarm::None;
    // An OPEN pulse always closes, before any gate below can say otherwise: a displaced view must
    // never be stranded by the period, the level cap, or the window lapsing mid-pulse.
    if (!in.pulseOpen) {
        if (in.maxLevel > 0 && in.applyLevel > (double)in.maxLevel) return TxWarm::None;
        if (in.windowMs > 0 && in.sinceLastChangeMs >= (unsigned long long)in.windowMs) return TxWarm::None;
        // Cadence: a new pulse only when the period has elapsed.
        if (in.warmHz > 0 && in.sinceLastWarmMs < 1000ull / (unsigned long long)in.warmHz) return TxWarm::None;
    }
    switch (in.mode) {
        case 1:  return TxWarm::Jitter1px;
        case 2:  return TxWarm::SameValue;
        case 3:  return TxWarm::InputTransform;
        default: return TxWarm::LevelEpsilon;
    }
}

}  // namespace wind
