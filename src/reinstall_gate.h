#pragma once
// Pure rate-limit for LL-keyboard-hook reinstall requests (issue #176).
//
// The #156 watchdog tell (async says a bind key is down, the hook says it is up) is
// satisfied FOREVER by a stranded async key, not just by a real eviction - and every
// reinstall opens an ~8 ms hook-inactive window in which the polling fallback reads the
// stranded key as held. Unlimited, that fired 4x/second for 7 minutes straight (the zoom
// crawl). The gate caps the cadence: at most one reinstall per 2 s, and after 3 inside a
// 30 s window a 30 s cooldown. A real eviction still heals on the first request (the gate
// starts open), and polling keeps the binds working between allowed requests - that is the
// existing fallback design. Pure logic (no windows.h) so the windowing is unit-tested.
namespace wind {

class ReinstallGate {
public:
    // May a reinstall be requested at nowMs? A true return RECORDS the request.
    bool allow(unsigned long long nowMs) {
        if (nowMs < cooldownUntilMs_) return false;
        if (winCount_ > 0 && nowMs - lastMs_ < kMinGapMs) return false;
        if (winCount_ > 0 && nowMs - winStartMs_ > kWindowMs) winCount_ = 0;   // window expired
        if (winCount_ >= kMaxPerWindow) {
            cooldownUntilMs_ = lastMs_ + kCooldownMs;
            winCount_ = 0;
            return false;
        }
        if (winCount_ == 0) winStartMs_ = nowMs;
        ++winCount_;
        lastMs_ = nowMs;
        return true;
    }
private:
    static constexpr unsigned long long kMinGapMs   = 2000;
    static constexpr unsigned long long kWindowMs   = 30000;
    static constexpr unsigned long long kCooldownMs = 30000;
    static constexpr int kMaxPerWindow = 3;
    unsigned long long lastMs_ = 0;          // last ALLOWED request
    unsigned long long winStartMs_ = 0;      // first allowed request of the current window
    unsigned long long cooldownUntilMs_ = 0;
    int winCount_ = 0;                       // allowed requests in the current window
};

}  // namespace wind
