#pragma once
namespace wind {

enum class PresentChoice { Blt, Dcomp };
// Why the last transition happened (for diagnostics logging). None = no transition this update.
enum class PresentReason { None, Throttle, Cue, Backstop };

// Pure policy for present=auto (issue #69). Chooses dcomp normally and falls back to blt when the
// flip-model composite is throttled (a windowed app over a background fullscreen game drops our
// on-screen rate to ~60), then re-probes dcomp when the state likely clears. Pure + hysteretic,
// like LockDetector; main.cpp feeds it per-tick signals and acts on choice() at a zoom boundary.
class PresentPolicy {
public:
    // dt                  : seconds since last tick (real, unclamped).
    // zoomed              : magnifier is zoomed this tick (we only present/measure then).
    // fps                 : measured loop fps this tick (<= 0 if unknown).
    // refreshHz           : detected display refresh (<= 0 falls back to 60).
    // foregroundFullscreen: the foreground window covers the target monitor (game likely in front).
    // foregroundChanged   : the foreground window handle changed since the previous tick.
    PresentChoice update(double dt, bool zoomed, double fps, int refreshHz,
                         bool foregroundFullscreen, bool foregroundChanged);
    PresentChoice choice() const { return choice_; }
    PresentReason lastReason() const { return reason_; }
    void reset();   // back to the optimistic initial state (Dcomp)

private:
    PresentChoice choice_ = PresentChoice::Dcomp;
    PresentReason reason_ = PresentReason::None;
    double belowSecs_  = 0.0;   // sustained time below the throttle threshold (while zoomed, in Dcomp)
    double sinceProbe_ = 0.0;   // time on Blt since the last dcomp probe (drives the backstop)
};

}
