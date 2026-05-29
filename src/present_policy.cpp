#include "present_policy.h"
namespace wind {
namespace {
constexpr double kThrottleFrac = 0.7;    // avg fps below this * refresh counts as throttled
constexpr double kWindowSecs   = 1.0;    // averaging window (zoomed, in Dcomp). 1s so a brief dip
                                         //   averages with surrounding good frames and does not trip;
                                         //   only a sustained ~1s of half-rate (the real throttle) does.
constexpr double kBackstopSecs = 60.0;   // re-probe dcomp at least this often while on Blt
}

void PresentPolicy::reset() {
    choice_ = PresentChoice::Dcomp;
    reason_ = PresentReason::None;
    winSecs_ = 0.0;
    winFrames_ = 0;
    sinceProbe_ = 0.0;
}

PresentChoice PresentPolicy::update(double dt, bool zoomed, int refreshHz,
                                    bool foregroundFullscreen, bool foregroundChanged) {
    if (dt < 0.0) dt = 0.0;
    reason_ = PresentReason::None;
    const double threshold = kThrottleFrac * (refreshHz > 0 ? refreshHz : 60);

    if (choice_ == PresentChoice::Dcomp) {
        // Measure the AVERAGE loop fps over a ~1s window while zoomed. The throttle is hitchy
        // (fast frames interleaved with slow ones), so averaging - not a per-frame test - is what
        // detects it. Reset the window when not zoomed (we only present/measure then).
        if (!zoomed) { winSecs_ = 0.0; winFrames_ = 0; return choice_; }
        winSecs_ += dt;
        winFrames_ += 1;
        if (winSecs_ >= kWindowSecs) {
            const double avgFps = winFrames_ / winSecs_;
            winSecs_ = 0.0;
            winFrames_ = 0;
            if (avgFps > 0.0 && avgFps < threshold) {
                choice_ = PresentChoice::Blt;
                reason_ = PresentReason::Throttle;
                sinceProbe_ = 0.0;
            }
        }
        return choice_;
    }

    // On Blt: re-probe dcomp when a fullscreen window just came to the foreground (game returned)
    // or the backstop elapses. Edge-triggered cue (needs a foreground CHANGE) so we do not probe
    // repeatedly while a fullscreen app sits in front.
    sinceProbe_ += dt;
    const bool cue = (foregroundFullscreen && foregroundChanged);
    if (cue || sinceProbe_ >= kBackstopSecs) {
        choice_ = PresentChoice::Dcomp;
        reason_ = cue ? PresentReason::Cue : PresentReason::Backstop;
        winSecs_ = 0.0;
        winFrames_ = 0;
    }
    return choice_;
}

}
