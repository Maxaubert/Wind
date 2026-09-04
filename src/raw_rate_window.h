#pragma once
// Locked-regime input speed over a trailing window - PURE (no <windows.h>), unit-tested.
//
// The learned pointer-ballistics gain (gain_learner.h) is indexed by input RATE, and it was
// learned from per-tick aggregates: raw counts per tick interval (6.94ms on the dev rig). Once
// the pan is also written between ticks (sub-tick pan, config.h subTickPan), every raw-input
// drain - a tick's aggregate or a single packet - needs a rate estimate on that same footing,
// or the two writers pan at different speeds. Per-drain dt was tried first and disagreed with
// the tick by 12-18% both ways: packets that queued while the tick ran arrived as 2-3 counts
// over a fraction of a millisecond (a speed several bins too high), and a plain sample window
// that reset every tick left the first packet after it alone (bins too low).
//
// Every drain is a sample spanning the time since the previous drain. Drains tile time, so the
// counts inside the last `window` are exactly the sum of each sample's counts in proportion to
// the part of its span that falls inside the window - identical whether the motion arrived as
// one tick aggregate or as five packets.
namespace wind {

class RawRateWindow {
public:
    static constexpr int kCap = 64;

    // Record one drain: `counts` raw mickeys arrived over (start, end]. Time units are the
    // caller's (QPC ticks); only differences matter.
    void push(long long start, long long end, double counts) {
        if (end <= start) start = end - 1;
        // Drop samples that end before the window could ever need them again is the caller's
        // window's job (countsIn); here just keep the ring bounded.
        if (n_ == kCap) { for (int i = 1; i < kCap; ++i) s_[i - 1] = s_[i]; n_ = kCap - 1; }
        s_[n_++] = Sample{ start, end, counts };
    }

    // Counts inside (end - window, end], then forget samples that ended before that window
    // (they can never overlap a later window, since `end` only moves forward).
    double countsIn(long long end, long long window) {
        const long long from = end - window;
        int keep = 0;
        for (int i = 0; i < n_; ++i)
            if (s_[i].end > from) s_[keep++] = s_[i];
        n_ = keep;
        double total = 0.0;
        for (int i = 0; i < n_; ++i) {
            const Sample& r = s_[i];
            const long long a = r.start > from ? r.start : from;
            const long long b = r.end < end ? r.end : end;
            if (b > a && r.end > r.start)
                total += r.counts * double(b - a) / double(r.end - r.start);
        }
        return total;
    }

    void reset() { n_ = 0; }
    int size() const { return n_; }

private:
    struct Sample { long long start, end; double counts; };
    Sample s_[kCap]{};
    int n_ = 0;
};

}  // namespace wind
