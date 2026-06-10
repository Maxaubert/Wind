#pragma once
namespace wind {

// Pure frame-skip support for the render engine (no <windows.h> - unit-tested). The engine
// renders only when something that affects the output image changed; otherwise it skips
// draw+present entirely, so DWM has nothing to recomposite and idle-zoomed GPU drops to ~0
// (the same render-on-change behavior that keeps Windows Magnifier at ~1%).

// Plain rect, same layout/meaning as Win32 RECT (fields: left, top, right, bottom).
// Half-open: x in [left, right), y in [top, bottom). Empty/inverted rects never intersect.
struct GateRect { long left, top, right, bottom; };

// True if a and b overlap.
bool RectsIntersect(const GateRect& a, const GateRect& b);

// Everything that affects the rendered image for one tick, reduced to comparable values.
// cursorShapeId is the HCURSOR value (opaque id; changes on shape swap). outlineAlpha is the
// EFFECTIVE alpha: 0 when the outline is disabled or not drawn at this level. Visual config
// knobs (bilinear, sharpness, ...) are deliberately NOT here; a config hot-reload forces one
// render instead (RunTick sets forceRender).
struct FrameSnapshot {
    double level = 0.0;
    double srcLeft = 0.0, srcTop = 0.0;
    double cursorScreenX = 0.0, cursorScreenY = 0.0;
    bool   cursorVisible = false;
    unsigned long long cursorShapeId = 0;
    float  outlineAlpha = 0.0f;
};

// True when rendering b would produce a visibly different image than already-presented a.
// Epsilons (named in the .cpp, covered by tests): source rect 1e-3 desktop px (at zoom 12
// that is 0.012 screen px - invisible, and it lets the cursor-smoothing tail settle instead
// of rendering forever); cursor 0.05 screen px; level 1e-9 (a ramp must always render).
bool SnapshotsDiffer(const FrameSnapshot& a, const FrameSnapshot& b);

// True when an acquired duplication frame is solely the DWM echo of our OWN previous Present.
// The overlay is capture-EXCLUDED (its pixels never appear in the captured image), but DWM
// still reports the overlay's window region as dirty in the next duplication frame after each
// of our presents. Treating that echo as a desktop change chains present -> dirty -> present
// forever, so the idle frame-skip gate never engages (it only broke when a tick's acquire
// happened to race the composite). Signature of the echo, all required: we presented since the
// last acquired image frame, exactly one accumulated composite (>1 may hide a real change
// merged in), and exactly ONE dirty rect exactly equal to the overlay rect (a real change in
// another window adds its own rect, and a partial change differs from the full overlay rect).
// `dirty0` is the first dirty rect; only inspected when dirtyCount == 1.
// CAVEAT: a fullscreen app repainting the whole monitor every refresh can ALIAS with this
// signature (its full-monitor dirty rect merges into the same composite as our echo), so the
// signature alone would skip every other real frame and halve the capture rate. EchoFilter
// below bounds that to ~120 ms of engagement: sustained real changes build a streak that
// bypasses the echo classification, so full-rate content keeps the full refresh rate.
bool IsPresentEcho(bool presentedSinceLastFrame, unsigned accumulatedFrames,
                   unsigned dirtyCount, const GateRect& dirty0, const GateRect& overlay);

// Hysteresis that tells a genuine idle echo from full-rate fullscreen content aliasing with the
// echo signature. Feed it one event per acquire attempt: a non-echo-shaped image change
// (noteRealChange), an AcquireNextFrame timeout (noteTimeout), or an echo-shaped frame
// (onEchoShaped, which also returns the classification). CRITICAL: echo-shaped frames never
// reset the real streak - while halved, the stream is real, echo-shaped, timeout, real, ... and
// the streak must build through them. Pointer-only frames (LastPresentTime == 0) must not touch
// the filter at all. Decision: once kEchoBypassStreak real changes accumulate (only full-rate
// content sustains that; idle changes like a clock are followed by long timeout runs, which
// reset), echo-shaped frames are treated as real so every game frame renders.
// Four empirically-driven details (verified against wind_diag.log on the 144 Hz panel):
//  - The tick loop polls FASTER than DWM composites (timer-paced skipped ticks at ~200 Hz vs
//    144 Hz vblank), so single WAIT_TIMEOUTs interleave even under full-rate content. Only
//    kEchoTimeoutReset CONSECUTIVE timeouts (a real content gap, ~20 ms) reset the streak.
//  - What clears the timeout run matters: real changes and BYPASSED echoes do (full-rate
//    cadence frames; stray loop hitches must not accumulate into a reset while engaged), but a
//    SKIPPED echo is neutral to it.
//  - SKIPPED echoes DECAY the streak when they pile up between reals (-1 per kEchoSkipDecay of
//    them since the last real; never a reset). The worlds differ in the real:skipped-echo
//    RATIO: the genuine halved regime alternates 1:1 (the decay window restarts on every real,
//    so the streak builds at full speed and engages in kEchoBypassStreak cycles, ~120 ms),
//    while an idle desktop's echo stream outnumbers its misclassified "reals" (late second
//    echoes of our own presents, merged accum>1 composites) by an order of magnitude, so decay
//    pins the streak at 0. Without the decay those sparse fake reals ratcheted the streak to
//    the threshold (timeouts alone were too sparse to reset between them) and ignited a
//    permanent present -> echo -> present chain on a static desktop.
//  - Once bypass engages, EVERY composite is echo-shaped (game dirt merges with our echo), so a
//    game that stops would leave a self-sustaining present -> echo -> present chain that never
//    times out. Every kEchoProbeInterval-th consecutive bypassed echo is deliberately classified
//    echo as a PROBE that also drops the streak just below the threshold: a live game re-proves
//    itself within a composite or two (the pre-probe present's echo can merge into the next one,
//    so re-proof can land one composite later; classified real -> re-engaged seamlessly), while
//    a stale chain stops presenting, its late-echo trickle decays the streak,
//    and the timeout run resets it, so idle skipping resumes within ~the interval instead of never.
//    Trade-off: 128 held one frame per ~0.9 s at 144 Hz (noticeable stale-view microstutter in
//    sensitive games); 512 extends to ~3.6 s apart, costs a longer bounded stale-chain tail after
//    content stops (idle-only, delays frame-skip onset by up to ~3.6 s, no visual effect).
// The engine also reset()s on fresh grabs (zoom-in/retarget) so a streak from a previous
// context never leaks into a new zoom session.
constexpr int kEchoBypassStreak   = 8;    // net streak needed to engage (~120 ms of halving)
constexpr int kEchoTimeoutReset   = 4;    // consecutive timeouts = content gap -> streak resets
constexpr int kEchoSkipDecay      = 2;    // skipped echoes since the last real per -1 decay
constexpr int kEchoProbeInterval  = 512;  // bypassed echoes between liveness-probe re-proofs
class EchoFilter {
public:
    void noteRealChange();                 // non-echo-shaped image change: streak builds (capped)
    void noteTimeout();                    // no new composite this poll; resets after a full run
    void reset();                          // new capture context: discard stale evidence
    // Classify an echo-shaped frame. True = treat it as a REAL change (render it); false =
    // classify as echo (skip). Never touches the real streak; maintains the probe cadence.
    bool onEchoShaped();
    int  realStreak() const { return realStreak_; }   // visibility for tests/diagnostics
private:
    int realStreak_ = 0;       // real changes since the last content gap / reset (echo-decayed)
    int timeoutRun_ = 0;       // consecutive timeouts (cleared by reals and bypassed echoes ONLY)
    int skipDecay_  = 0;       // skipped echoes since the last -1 streak decay
    int bypassRun_  = 0;       // consecutive bypassed echoes since the last real change / probe
};

}
