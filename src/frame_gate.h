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

// True when an acquired duplication frame is SHAPED like the DWM echo of our OWN previous
// Present. The overlay is capture-EXCLUDED (its pixels never appear in the captured image), but
// DWM still reports the overlay's window region as dirty in the next duplication frame after
// each of our presents. Treating that echo as a desktop change chains present -> dirty ->
// present forever, so the idle frame-skip gate never engages (it only broke when a tick's
// acquire happened to race the composite). Signature, both required: we presented since the
// last acquired image frame, and the dirty rects cover >= kEchoCoverageNum/kEchoCoverageDen of
// the overlay's area. COVERAGE, not exact rect equality: the observed echo shapes on a real
// desktop are the exact overlay rect, the overlay MINUS a higher-band window's strip (the
// taskbar's opaque region is subtracted from our dirty contribution, ~95% coverage), and that
// same union split into two rects (work area + taskbar) - exact-equality classified the latter
// two as real changes, which re-armed the grace below forever and the post-present chain never
// died. AccumulatedFrames is deliberately NOT part of the signature either: while we present
// every tick, two of our own echoes can merge into one accum>1 composite with the same shape.
// A real change covering >= 80% of the monitor is exactly the aliasing class the grace below
// arbitrates anyway; smaller changes (windows, dialogs, carets) stay real by area.
// `dirtyArea` is the summed area of the frame's dirty rects (DDA dirty rects do not overlap).
// CAVEAT: any real change that DWM merges into the same composite as our echo ALIASES with this
// signature (the overlay region covers the whole monitor, so the union of echo + change is
// still the echo's shape): fullscreen apps repainting every refresh, but also SPORADIC
// transitions (alt-tab commit, Start menu close) whose repaint rides the echo of the present
// they triggered. The signature alone would swallow those. EchoFilter below resolves the alias
// with a recent-activity grace: echo-shaped frames render while real activity is fresh and are
// skipped only once the desktop has actually gone quiet.
constexpr long long kEchoCoverageNum = 4;   // dirty union covering >= 4/5 of the overlay area
constexpr long long kEchoCoverageDen = 5;   // = echo-shaped (taskbar-clipped echoes are ~95%)
bool IsPresentEcho(bool presentedSinceLastFrame, long long dirtyArea, long long overlayArea);

// Hysteresis + grace that arbitrate echo-shaped frames. Feed it one event per acquire attempt:
// a non-echo-shaped image change (noteRealChange), an AcquireNextFrame timeout (noteTimeout),
// or an echo-shaped frame (onEchoShaped, which also returns the classification). The engine
// feeds noteRealChange ONLY from changes whose dirty/move rects intersect the magnified view
// (or the conservative no-metadata paths); a frame whose changes are all OFF-VIEW is neutral to
// the filter, so content animating elsewhere on the monitor cannot engage it and defeat idle
// skipping over a static view (issue #96). Pointer-only frames (LastPresentTime == 0) must not
// touch the filter at all.
// An echo-shaped frame is treated as REAL (rendered) when EITHER of two independent mechanisms
// says so; otherwise it is skipped (and the engine schedules a one-shot catch-up render - see
// catchUpPending in render_engine.cpp - so even a skipped composite's merged content reaches
// the screen one tick late; that closes the classic swallow hole at every chain death).
//
// MECHANISM 1 - the real-change STREAK (sustained full-rate content). A fullscreen app
// repainting every refresh merges its dirty region into the same composite as our echo and
// aliases with the echo signature; skipping those halves the capture rate (forbidden in-game).
// Once kEchoBypassStreak real changes accumulate without a content gap, echo-shaped frames are
// bypassed (treated real) so every game frame renders. Empirically-driven details (verified
// against wind_diag.log on the 144 Hz panel):
//  - The tick loop polls FASTER than DWM composites, so single WAIT_TIMEOUTs interleave even
//    under full-rate content. Only kEchoTimeoutReset CONSECUTIVE timeouts (a real content gap,
//    ~20 ms) reset the streak; real changes and BYPASSED echoes (streak- or grace-) clear the
//    run, but a SKIPPED echo is neutral to it.
//  - SKIPPED echoes DECAY the streak when they pile up between reals (-1 per kEchoSkipDecay
//    since the last real): the full-rate regime's real:skipped ratio is ~1:0 (grace bypasses
//    the merges), while an idle desktop's sparse fake reals are outnumbered by their own echo
//    tails and the streak pins at 0 instead of ratcheting up.
//  - Once bypass engages, EVERY composite is echo-shaped (game dirt merges with our echo), so a
//    game that stops would leave a self-sustaining present -> echo -> present chain. Every
//    kEchoProbeInterval-th consecutive bypassed echo is deliberately skipped as a PROBE that
//    drops the streak just below the threshold: a live game re-proves itself on the next
//    composite (and the catch-up render shows the probe-skipped frame one tick late, so the
//    probe no longer even holds a frame); a stale chain stops presenting and the timeout run
//    resets the streak, so idle skipping resumes.
//
// MECHANISM 2 - the recent-activity GRACE (sporadic transitions; issue #96 second round). The
// echo signature is IDENTICAL for a pure echo and for a real change that DWM merged into the
// same composite as our echo (alt-tab commit, Start menu open/close). Such a merge can only
// exist within one composite of our own present, i.e. immediately after real activity, so: the
// first kEchoGraceTicks echo-shaped frames after a non-echo-shaped real change are treated as
// REAL. Only noteRealChange refreshes the grace; echo-shaped frames burn it down (timeouts do
// not - it is a per-change budget, not a wall-clock window) and an echo-triggered render must
// NOT refresh it, or the present -> echo -> present chain would never die. The streak machinery
// alone swallowed
// these sporadic merges unboundedly (streak < threshold for an isolated transition, and if the
// merged repaint was the LAST change the stale picker/menu stuck until the next unrelated
// change); the grace alone (without the streak) amplified every ambient few-Hz desktop change
// into a grace-window-long echo-render chain and defeated idle skipping. The hybrid keeps both
// regimes correct, and the catch-up render covers the residual one-composite hole where the
// grace expires exactly on a transition's final merged repaint.
// Cost accounting: a sporadic change now costs 1 + kEchoGraceTicks + 1 (catch-up) presents
// (~6); sustained content runs at full rate (streak bypass); after content stops the chain dies
// within the grace + the timeout run, and idle skipping resumes.
// The engine also reset()s on fresh grabs (zoom-in/retarget) so evidence from a previous
// context never leaks into a new zoom session.
constexpr int kEchoBypassStreak   = 8;    // net streak needed to engage the full-rate bypass
constexpr int kEchoTimeoutReset   = 6;    // consecutive timeouts = content gap -> streak and
                                          // grace reset (6, not 4: the post-skip suspect
                                          // stretch interleaves up to ~4 timeouts through
                                          // skip-neutral echoes at the ~205-260 Hz poll rate,
                                          // and that must not reset a mid-build streak; a
                                          // genuine gap - wallpaper cadence, blink interval -
                                          // is far longer than 6 polls)
constexpr int kEchoSkipDecay      = 6;    // skipped echoes since the last real per -1 decay
                                          // (6, not 2: each build-up cycle in the fully-merged
                                          // regime skips 4 frames - one grace-exhausted skip
                                          // plus the echo-budget drain after the catch-up - and
                                          // those must not erase the cycle's +1 or the bypass
                                          // never engages; the idle echo stream still
                                          // outnumbers fake reals far beyond 6:1, and ambient
                                          // few-Hz content is reset by its quiet gaps via
                                          // kEchoTimeoutReset, so the streak stays pinned at 0)
constexpr int kEchoProbeInterval  = 512;  // bypassed echoes between liveness-probe re-proofs
constexpr int kEchoGraceTicks     = 4;    // echo-shaped frames rescued after each real change
class EchoFilter {
public:
    void noteRealChange();                 // non-echo-shaped view-relevant change: streak builds,
                                           // grace refreshes
    void noteTimeout();                    // no new composite this poll: a full consecutive run
                                           // resets the streak (grace untouched)
    void reset();                          // new capture context: discard stale evidence
    // Classify an echo-shaped frame. True = treat it as a REAL change (render it); false =
    // classify as echo (skip; the engine then schedules the one-shot catch-up render).
    bool onEchoShaped();
    int  realStreak() const { return realStreak_; }   // visibility for tests/diagnostics
    int  graceLeft()  const { return graceLeft_;  }
private:
    int realStreak_ = 0;   // real changes since the last content gap / reset (skip-decayed)
    int timeoutRun_ = 0;   // consecutive timeouts (cleared by reals and bypassed echoes ONLY)
    int skipDecay_  = 0;   // skipped echoes since the last -1 streak decay
    int bypassRun_  = 0;   // consecutive streak-bypassed echoes since the last real / probe
    int graceLeft_  = 0;   // events left in the merge-rescue window after a real change
};

}
