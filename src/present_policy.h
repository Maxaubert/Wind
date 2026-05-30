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
    // dt                  : seconds since last tick (real, unclamped). Used to measure the loop rate.
    // zoomed              : magnifier is zoomed this tick (we only present/measure then).
    // refreshHz           : detected display refresh (<= 0 falls back to 60).
    // foregroundFullscreen: the foreground window covers the target monitor (game likely in front).
    // foregroundChanged   : the foreground window handle changed since the previous tick.
    // Detection uses the AVERAGE loop fps over a short window (frames/elapsed), NOT per-frame fps:
    // the bad-state throttle is hitchy (fast and slow frames alternate, averaging ~half rate), so a
    // per-frame "consecutive below threshold" test never trips. A windowed average catches it.
    PresentChoice update(double dt, bool zoomed, int refreshHz,
                         bool foregroundFullscreen, bool foregroundChanged);
    PresentChoice choice() const { return choice_; }
    PresentReason lastReason() const { return reason_; }
    void reset();   // back to the optimistic initial state (Dcomp)

private:
    PresentChoice choice_ = PresentChoice::Dcomp;
    PresentReason reason_ = PresentReason::None;
    double winSecs_   = 0.0;   // accumulated real time in the current measurement window (zoomed, Dcomp)
    int    winFrames_ = 0;     // ticks in the current window; avg fps = winFrames_ / winSecs_
    double sinceProbe_ = 0.0;  // time on Blt since the last dcomp probe (drives the backstop)
};

}
