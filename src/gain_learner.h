#pragma once
// Learned pointer-ballistics gain - PURE (no <windows.h>), unit-tested.
//
// THE PROBLEM. Locked-regime panning (mouselook games, lockApps) cannot use the OS cursor as its
// speed oracle - the game confines or warps the pointer - so it pans from raw HID mickeys. Raw
// mickeys miss everything Windows applies to the real cursor: the pointer-speed slider, the
// "Enhance pointer precision" curve, and a set of undocumented device/polling-rate dependent
// scaling constants. Every attempt to MODEL that pipeline here has needed fudge factors
// (inputDiv 3.5 -> 6, accelStrength 0.3, then 0.8), and each landed either slower or faster than
// the desktop because the model's speed estimate is wrecked by WM_INPUT coalescing, whose degree
// depends on the mouse's polling rate and the tick rate.
//
// THE FIX: do not model - MEASURE. Whenever the cursor is FREE, Wind already sees both ends of
// Windows' own pipeline every tick: raw mickeys in (Raw Input), OS cursor movement out
// (GetCursorPos). gain = out/in at a given input rate IS the user's effective ballistics, with
// the slider, the curve, the polling rate and all the undocumented constants baked in, because
// Windows itself computed it. This class records that ratio into log-spaced speed bins while the
// cursor is free, and the locked path replays it: pan speed == desktop cursor speed, by
// construction, for THIS user's exact mouse and settings. No knobs.
//
// Robustness choices, each load-bearing:
//   - EMA per bin, not an average: settings changes (slider moved, EPP toggled) re-converge in
//     seconds instead of being diluted by history.
//   - samples gated by the CALLER (no clip, cursor away from screen edges, no injection running):
//     a clamped cursor under-reports out-pixels and would teach a too-low gain.
//   - minimum input per sample: a 1-2 mickey tick quantizes to garbage ratios.
//   - until a bin has data, fall back to the nearest LEARNED bin; until anything is learned,
//     gain 1.0 (raw passthrough - the pre-learning behavior).
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace wind {

class GainLearner {
public:
    static constexpr int    kBins      = 12;
    static constexpr double kMinRate   = 0.25;   // mickeys/ms at the bottom of bin 0
    static constexpr double kMaxRate   = 64.0;   // top of the last bin; flicks beyond clamp here
    static constexpr double kAlpha     = 0.05;   // EMA weight: ~60 samples to converge a bin
    static constexpr int    kMinCounts = 3;      // ignore ticks with fewer input mickeys

    // One free-cursor observation: `counts` raw mickeys arrived over `dtMs`, and the OS cursor
    // moved `outPx`. The caller has already gated for clips / edges / injection.
    void observe(double counts, double outPx, double dtMs) {
        if (counts < kMinCounts || dtMs <= 0.0 || outPx < 0.0) return;
        const double g = outPx / counts;
        if (g <= 0.01 || g > 40.0) return;       // impossible ratio: clamped cursor or glitch
        const int b = bin(counts / dtMs);
        if (n_[b] == 0) { gain_[b] = g; n_[b] = 1; }
        else            { gain_[b] += (g - gain_[b]) * kAlpha; if (n_[b] < 1000000) ++n_[b]; }
    }

    // Gain to apply to a locked-regime packet arriving at `counts` mickeys over `dtMs`.
    // INTERPOLATED between bins (2026-08-28): a flat per-bin gain made pan speed jump discretely
    // every time the hand crossed a bin boundary - field-reported as stepping, worst while
    // ramping and panning together. Piecewise-linear over the log-rate axis keeps the replayed
    // curve as continuous as the real one.
    double gainFor(double counts, double dtMs) const {
        if (dtMs <= 0.0) return fallback();
        const double pos = binPos(counts / dtMs);
        // Interpolate between the nearest LEARNED bins on each side (bin centres as sample
        // points). Interpolating between raw neighbours and nearest-filling the gaps was tried
        // first and left a cliff at the midpoint of every unlearned gap - the same discontinuity
        // this exists to remove. Sparse data now yields one continuous piecewise-linear curve.
        int lo = -1, hi = -1;
        for (int i = 0; i < kBins; ++i) {
            if (n_[i] == 0) continue;
            const double c = i + 0.5;
            if (c <= pos) lo = i;
            if (c >= pos && hi < 0) hi = i;
        }
        if (lo < 0 && hi < 0) return 1.0;             // nothing learned yet: raw passthrough
        if (lo < 0) return gain_[hi];                 // below the lowest learned centre
        if (hi < 0) return gain_[lo];                 // above the highest learned centre
        if (lo == hi) return gain_[lo];
        const double t = (pos - (lo + 0.5)) / (double)(hi - lo);
        return gain_[lo] + (gain_[hi] - gain_[lo]) * (t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t));
    }

    bool warmedUp() const {
        int have = 0;
        for (int i = 0; i < kBins; ++i) if (n_[i] > 0) ++have;
        return have >= 2;
    }

    void reset() { for (int i = 0; i < kBins; ++i) { gain_[i] = 1.0; n_[i] = 0; } }

    // Text round-trip, so a restart starts from the last learned curve instead of raw passthrough
    // ("default to the last read value"). Format: one "gain count" pair per line, kBins lines.
    // Pure string I/O here; the caller owns the file.
    void serialize(char* out, int cap) const {
        int off = 0;
        for (int i = 0; i < kBins && off < cap - 32; ++i) {
            int w = 0;
            // %g keeps it short; counts capped on write so an ancient file cannot pin the EMA
            const int nc = n_[i] > 1000 ? 1000 : n_[i];
            w = snprintf(out + off, (size_t)(cap - off), "%.6g %d\n", gain_[i], nc);
            if (w <= 0) break;
            off += w;
        }
        if (off < cap) out[off] = 0; else out[cap - 1] = 0;
    }
    bool deserialize(const char* in) {
        if (!in) return false;
        double g[kBins]; int c[kBins];
        const char* pIn = in;
        for (int i = 0; i < kBins; ++i) {
            char* end = nullptr;
            g[i] = std::strtod(pIn, &end);
            if (end == pIn) return false;
            pIn = end;
            c[i] = (int)std::strtol(pIn, &end, 10);
            if (end == pIn) return false;
            pIn = end;
            if (!(g[i] > 0.01 && g[i] <= 40.0) || c[i] < 0) return false;   // corrupt: reject whole file
        }
        for (int i = 0; i < kBins; ++i) { gain_[i] = g[i]; n_[i] = c[i]; }
        return true;
    }

private:
    static double binPos(double rate) {
        if (rate <= kMinRate) return 0.0;
        if (rate >= kMaxRate) return (double)(kBins - 1);
        // log-spaced: pointer ballistics vary most at low speed, so low bins must be narrow
        const double t = std::log(rate / kMinRate) / std::log(kMaxRate / kMinRate);
        return t * kBins;
    }
    static int bin(double rate) {
        int b = (int)binPos(rate);
        if (b < 0) b = 0;
        if (b >= kBins) b = kBins - 1;
        return b;
    }
    double fallback() const {
        // No timing: average of learned bins, else 1.0.
        double s = 0.0; int c = 0;
        for (int i = 0; i < kBins; ++i) if (n_[i] > 0) { s += gain_[i]; ++c; }
        return c > 0 ? s / c : 1.0;
    }

    double gain_[kBins] = { 1,1,1,1,1,1,1,1,1,1,1,1 };
    int    n_[kBins]    = {};
};

}  // namespace wind
