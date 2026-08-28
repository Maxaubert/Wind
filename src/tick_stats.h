#pragma once
// Recent tick intervals, for the tray's frame-pacing readout. PURE (no <windows.h>) so the
// statistics are unit-testable without a desktop.
//
// The tray needs to show whether the loop is keeping up. The loop already computes `dt` every tick
// for the diagnostics log, so the only new cost here is ONE float store per tick - no allocation,
// no lock, no branch worth measuring. That matters: this is the magnifier's hot path, and a status
// readout must never be the reason a frame is late.
//
// Single producer (the tick loop), single consumer (the tray, when the menu opens). The ring is
// deliberately not synchronised beyond a relaxed head counter: a torn read costs one wrong pixel in
// a sparkline and nothing else, which is not worth a lock on the tick path.
#include <atomic>
#include <algorithm>

namespace wind {

class TickStats {
public:
    static constexpr int kCap = 256;      // ~1.8s at 144Hz - enough to see a stall, cheap to hold

    // Tick thread. One relaxed store plus one relaxed increment.
    void push(float ms) {
        const unsigned h = head_.load(std::memory_order_relaxed);
        buf_[h % kCap] = ms;
        head_.store(h + 1, std::memory_order_relaxed);
    }

    // Any thread. Copies the newest `max` samples oldest-first; returns how many were written.
    int snapshot(float* out, int max) const {
        if (!out || max <= 0) return 0;
        const unsigned h = head_.load(std::memory_order_relaxed);
        const int have = (int)(h < (unsigned)kCap ? h : (unsigned)kCap);
        const int n = have < max ? have : max;
        for (int i = 0; i < n; ++i) out[i] = buf_[(h - (unsigned)(n - i)) % kCap];
        return n;
    }

    bool empty() const { return head_.load(std::memory_order_relaxed) == 0; }
    void reset() { head_.store(0, std::memory_order_relaxed); }

private:
    float buf_[kCap]{};
    std::atomic<unsigned> head_{0};
};

// Process-wide instance. Defined inline so the header is self-contained (C++17).
inline TickStats& Ticks() { static TickStats s; return s; }

// --- pure statistics, unit-tested ---------------------------------------------------------

// Median of the sample window. Median, not mean: one 25ms stall must not drag the headline figure
// the way it would an average, because the headline is answering "is it keeping up right now".
inline double MedianMs(const float* v, int n) {
    if (!v || n <= 0) return 0.0;
    float tmp[TickStats::kCap];
    const int m = n < TickStats::kCap ? n : TickStats::kCap;
    for (int i = 0; i < m; ++i) tmp[i] = v[i];
    std::sort(tmp, tmp + m);
    return (double)tmp[m / 2];
}

// Mean of the sample window - the rate estimator for the fps FIGURE. The mean, not the median:
// scheduler wake jitter comes in late/short PAIRS that sum to the true elapsed time, so they
// cancel in a mean, while the median sits on whichever half of the pair is more common - which
// is how the tray read "145 fps" on a 144Hz panel (field 2026-08-28). A lone stall barely moves
// a 256-sample mean (~0.07ms), and a SUSTAINED slowdown moving it is the figure being honest.
// The median stays the anchor for the sparkline scale and the LateCount threshold, where
// robustness against stalls is exactly what is wanted.
inline double MeanMs(const float* v, int n) {
    if (!v || n <= 0) return 0.0;
    double s = 0.0;
    for (int i = 0; i < n; ++i) s += (double)v[i];
    return s / n;
}

// Frames per second implied by an interval. 0 in, 0 out - never a divide by zero into the UI.
inline double FpsFromMs(double ms) { return ms > 0.0001 ? 1000.0 / ms : 0.0; }

// How many of the samples were LATE: over 1.5x the median. The same 1.5x threshold the pan-wake
// harness uses, so the tray and the diagnostics agree about what counts as a stall.
inline int LateCount(const float* v, int n) {
    if (!v || n <= 0) return 0;
    const double thr = MedianMs(v, n) * 1.5;
    int late = 0;
    for (int i = 0; i < n; ++i) if ((double)v[i] > thr) ++late;
    return late;
}

}  // namespace wind
