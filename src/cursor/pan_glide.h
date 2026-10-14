#pragma once
// Pan glide (issue #430) - PURE, no <windows.h>, tests/test_pan_glide.cpp.
//
// A soft stop: when the hand stops a mouse movement, the POINTER eases on in the same direction for
// a few pixels instead of halting dead (exponential decay, like the zoom's release glide). It starts
// at exactly the hand's speed and only slows down, and it never travels further than a cap set in
// SCREEN pixels (field 2026-10-10: uncapped momentum could throw the pointer across the screen; what
// is wanted is an ease, "some pixels"). A faster hand eases out more steeply inside the same cap.
// The view follows by DWM centring exactly as it follows the hand, so nothing jumps when the hand
// takes over again, and every click lands where the pointer is seen.
//
// The caller feeds one tick of HAND motion at a time (never Wind's own glide steps), and cancels an
// active glide the moment the hand moves, a button is pressed, or the session stops being a plain
// zoomed desktop pan (game, Inspect, a detached view, 1x).
#include <cmath>
namespace wind {

struct PanGlide {
    double vx = 0.0, vy = 0.0;      // hand velocity estimate (desktop px/s), only while moving
    bool   movedLast = false;       // the previous tick had hand motion
    bool   active = false;          // a glide is running
    double t = 0.0;                 // seconds since it started
    double ox = 0.0, oy = 0.0;      // where the hand stopped
    double v0x = 0.0, v0y = 0.0;    // velocity it started with
    double tau = 0.0;               // decay time constant (s)
};

inline constexpr double kGlideVelTauS = 0.03;          // velocity estimate: the last ~30 ms of motion
inline constexpr double kGlideMinStartSpeed = 40.0;    // px/s: below this the hand was resting anyway
inline constexpr double kGlideStopDist = 0.5;          // desktop px left to travel: the glide is over

// One setting drives the glide (#434): its ease time grows with its distance, so a longer glide also
// slows down more gently. 40 px gives 60 ms, the old separate default.
inline double PanGlideTauS(int maxPx) {
    double ms = 40.0 + maxPx * 0.5;
    if (ms > 240.0) ms = 240.0;
    return ms / 1000.0;
}

// One tick of hand motion (dx, dy desktop px over dt s), with the pointer now at (curX, curY).
// Starts a glide (returns true) on the first still tick after motion when the session allows it.
// tauS is the ease's time constant; maxDist (desktop px, > 0) caps how far it travels: a hand fast
// enough to exceed it gets a shorter time constant instead, so the start speed is always the hand's.
// A still tick clears the estimate, so every movement is measured fresh.
inline bool PanGlideObserve(PanGlide& g, double dx, double dy, double dt, double curX, double curY,
                            double tauS, double maxDist, bool eligible) {
    if (dt <= 0.0) dt = 1.0 / 144.0;
    const bool moved = dx != 0.0 || dy != 0.0;
    if (moved) {
        const double k = 1.0 - std::exp(-dt / kGlideVelTauS);
        g.vx += (dx / dt - g.vx) * k;
        g.vy += (dy / dt - g.vy) * k;
    }
    bool started = false;
    if (!moved && g.movedLast && eligible && tauS > 0.0 && maxDist > 0.0 &&
        std::hypot(g.vx, g.vy) >= kGlideMinStartSpeed) {
        g.active = true;
        g.t = 0.0;
        g.ox = curX; g.oy = curY;
        g.v0x = g.vx; g.v0y = g.vy;
        const double speed = std::hypot(g.vx, g.vy);
        g.tau = (maxDist > 0.0 && speed * tauS > maxDist) ? maxDist / speed : tauS;
        started = true;
    }
    if (!moved) { g.vx = 0.0; g.vy = 0.0; }
    g.movedLast = moved;
    return started;
}

// Advance a running glide by dt: (x, y) is where the pointer belongs now. Ends the glide once less
// than kGlideStopDist is left to travel (the position returned on that tick is still valid).
inline bool PanGlideStep(PanGlide& g, double dt, double& x, double& y) {
    if (!g.active) return false;
    if (dt <= 0.0) dt = 1.0 / 144.0;
    g.t += dt;
    const double decay = std::exp(-g.t / g.tau);
    x = g.ox + g.v0x * g.tau * (1.0 - decay);
    y = g.oy + g.v0y * g.tau * (1.0 - decay);
    if (std::hypot(g.v0x, g.v0y) * g.tau * decay < kGlideStopDist) g.active = false;
    return true;
}

inline void PanGlideCancel(PanGlide& g) {
    g.active = false;
    g.vx = g.vy = 0.0;
    g.movedLast = false;
}

}  // namespace wind
