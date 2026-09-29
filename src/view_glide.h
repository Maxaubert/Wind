#pragma once
// Glide and target geometry for tracking (issue #276). Pure; tests/test_view_glide.cpp.
#include <cmath>
namespace wind {

// Time-based ease: 95% of the gap in glideMs whatever the tick interval (VRR-safe, the same
// reasoning as CursorMapper::setTickDeltaMs). keep = 0.05^(dt/glideMs).
inline double GlideToward(double cur, double target, double dtMs, double glideMs) {
    if (glideMs <= 0.0) return target;
    if (dtMs <= 0.0) return cur;
    const double keep = std::pow(0.05, dtMs / glideMs);
    return target + (cur - target) * keep;
}

// Critically damped spring (SmoothDamp form, stable at any dt). Unlike GlideToward it CARRIES its
// velocity across retargets, so a caret stepping one character per keystroke becomes one continuous
// glide instead of a fresh jolt per key (field ask 2026-09-29: "glide more as you type, never lag").
// glideMs is matched to GlideToward's: 95% of a step in glideMs (critically damped: ~4.74/omega).
inline double SpringToward(double cur, double target, double& vel, double dtMs, double glideMs) {
    if (glideMs <= 0.0) { vel = 0; return target; }
    if (dtMs <= 0.0) return cur;
    const double omega = 4.74 / (glideMs / 1000.0), dt = dtMs / 1000.0;
    const double x = omega * dt;
    const double decay = 1.0 / (1.0 + x + 0.48 * x * x + 0.235 * x * x * x);
    const double change = cur - target;
    const double temp = (vel + omega * change) * dt;
    vel = (vel - omega * temp) * decay;
    return target + (change + temp) * decay;
}

struct TrackRect { double l, t, r, b; };

inline bool TrackTargetCenter(const TrackRect& rc, double curCx, double curCy, double level,
                              int monW, int monH, int align, int marginPct,
                              double& outCx, double& outCy) {
    if (level < 1.0) level = 1.0;
    const double w = rc.r - rc.l, h = rc.b - rc.t;
    if (w < 0 || h < 0 || (w == 0 && h == 0)) return false;                 // empty / inverted
    if (rc.l == 0 && rc.t == 0 && rc.r == 0 && rc.b == 0) return false;      // the classic bogus caret
    if (rc.r < 0 || rc.b < 0 || rc.l > monW || rc.t > monH) return false;    // not on this monitor
    const double vw = monW / level, vh = monH / level;
    double cx = curCx, cy = curCy;
    if (align == 0) {
        cx = (rc.l + rc.r) / 2.0;
        cy = (rc.t + rc.b) / 2.0;
    } else {
        const double mx = vw * (marginPct / 100.0), my = vh * (marginPct / 100.0);
        auto axis = [](double c, double lo, double hi, double v, double m) {
            const double inLo = c - v / 2 + m, inHi = c + v / 2 - m;   // the comfortable band
            if (hi - lo > inHi - inLo) return lo - m + v / 2;          // oversized: align its start
            if (lo < inLo) return c - (inLo - lo);
            if (hi > inHi) return c + (hi - inHi);
            return c;
        };
        cx = axis(curCx, rc.l, rc.r, vw, mx);
        cy = axis(curCy, rc.t, rc.b, vh, my);
    }
    // Clamp so the view stays on the monitor (ComputeOffsetF clamps too; keeping the centre
    // consistent avoids a glide that aims past the edge and then stalls).
    const double minX = vw / 2, maxX = monW - vw / 2, minY = vh / 2, maxY = monH - vh / 2;
    outCx = cx < minX ? minX : (cx > maxX ? maxX : cx);
    outCy = cy < minY ? minY : (cy > maxY ? maxY : cy);
    return true;
}
}  // namespace wind
