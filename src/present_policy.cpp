#include "present_policy.h"
namespace wind {
namespace {
constexpr double kThrottleFrac    = 0.7;   // fps below this * refresh counts as throttled
constexpr double kThrottleConfirm = 1.0;   // sustained seconds below threshold -> fall back to Blt
constexpr double kBackstopSecs    = 60.0;  // re-probe dcomp at least this often while on Blt
}

void PresentPolicy::reset() {
    choice_ = PresentChoice::Dcomp;
    reason_ = PresentReason::None;
    belowSecs_ = 0.0;
    sinceProbe_ = 0.0;
}

PresentChoice PresentPolicy::update(double dt, bool zoomed, double fps, int refreshHz,
                                    bool foregroundFullscreen, bool foregroundChanged) {
    if (dt < 0.0) dt = 0.0;
    reason_ = PresentReason::None;
    const double threshold = kThrottleFrac * (refreshHz > 0 ? refreshHz : 60);

    if (choice_ == PresentChoice::Dcomp) {
        // Detect the throttle: sustained low loop fps while actually zoomed. A brief dip resets,
        // so only a real ~1s stall trips it. (Also catches a failed probe: re-throttle -> Blt.)
        if (zoomed && fps > 0.0 && fps < threshold) belowSecs_ += dt;
        else                                        belowSecs_ = 0.0;
        if (belowSecs_ >= kThrottleConfirm) {
            choice_ = PresentChoice::Blt;
            reason_ = PresentReason::Throttle;
            belowSecs_ = 0.0;
            sinceProbe_ = 0.0;
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
        belowSecs_ = 0.0;
        sinceProbe_ = 0.0;
    }
    return choice_;
}

}
