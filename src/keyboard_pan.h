#pragma once
// Keyboard panning (issue #287). A press starts panning at once (short ease-in, no jump). A press
// released within 250 ms is a TAP: that axis stops and is topped up to exactly one nudge (1/8 of
// the screen), which glides in quickly. A longer hold just pans and glides out on release; it never
// adds the nudge (owner field test 2026-09-30: the nudge at the start of a hold read as a jump).
// Rates are in SCREEN space so the feel is the same at every zoom; step() returns desktop-pixel
// deltas. Pure; tests/test_keyboard_pan.cpp. Key index order: Left, Right, Up, Down.
#include <cmath>
namespace wind {
struct KeyPan {
    static constexpr double kNudgeFrac = 0.125;      // a tap: 1/8 of the screen
    static constexpr double kTapMs = 250.0;          // released sooner than this: a tap
    static constexpr double kScreensPerSec = 1.25;   // continuous rate at panSpeed 1.0, 6x and above
    // Lower zoom pans slower (owner, 2026-09-30, #305): a constant screen rate crosses the whole
    // desktop in (level - 1) / rate seconds, under one at 2x. 100% at 6x+, 60% at 2x, linear between
    // (75% at 2x was "much better, could be a bit slower still").
    static double ZoomRateScale(double level) {
        const double s = 0.4 + 0.1 * level;
        return s > 1.0 ? 1.0 : s;
    }
    static constexpr double kEaseInMs = 150.0, kGlideMs = 120.0, kNudgeGlideMs = 90.0;

    double heldMs[4] = { 0, 0, 0, 0 };
    double travelled[4] = { 0, 0, 0, 0 };   // screen px this press has moved its axis, in its direction
    double nudgeX = 0, nudgeY = 0;          // screen px of tap top-ups still to travel
    double vx = 0, vy = 0;                  // continuous velocity, screen px/s

    void reset() { *this = KeyPan{}; }
    bool active() const {
        return heldMs[0] > 0 || heldMs[1] > 0 || heldMs[2] > 0 || heldMs[3] > 0 ||
               std::fabs(nudgeX) > 0.5 || std::fabs(nudgeY) > 0.5 || vx != 0 || vy != 0;
    }
    void step(const bool held[4], double dtMs, double level, int monW, int monH, double speed,
              double& dx, double& dy) {
        dx = dy = 0;
        if (level < 1.0) level = 1.0;
        if (dtMs <= 0) return;
        (void)monH;   // kept in the signature: the tap/rate span is the width for both axes
        static const double kSign[4] = { -1, 1, -1, 1 };
        double tvx = 0, tvy = 0;
        for (int i = 0; i < 4; ++i) {
            const bool horiz = i < 2;
            // One pixel rate and one tap step for every direction, from the screen WIDTH: on a
            // 16:9 panel a height-based rate made up/down pan at 56% of left/right (field test).
            const double span = monW;
            if (held[i]) {
                heldMs[i] += dtMs;
                (horiz ? tvx : tvy) += kSign[i] * speed * kScreensPerSec * ZoomRateScale(level) * span;
                continue;
            }
            if (heldMs[i] > 0 && heldMs[i] < kTapMs) {
                // A tap just ended: stop this axis and top it up to exactly one nudge.
                double& v = horiz ? vx : vy;
                v = 0;
                const double rest = kNudgeFrac * span - travelled[i];
                if (rest > 0) (horiz ? nudgeX : nudgeY) += kSign[i] * rest;
            }
            heldMs[i] = 0; travelled[i] = 0;
        }
        // Velocity chases its target: ~150 ms to full speed, ~120 ms to rest (3 time constants).
        auto chase = [&](double& v, double target) {
            const double a = 1.0 - std::exp(-dtMs / ((target != 0 ? kEaseInMs : kGlideMs) / 3.0));
            v += (target - v) * a;
            if (target == 0 && std::fabs(v) < 1.0) v = 0;
        };
        chase(vx, tvx); chase(vy, tvy);
        const double mx = vx * dtMs / 1000.0, my = vy * dtMs / 1000.0;
        for (int i = 0; i < 4; ++i) {
            if (!held[i]) continue;
            const double m = (i < 2 ? mx : my) * kSign[i];
            if (m > 0) travelled[i] += m;
        }
        // Tap top-ups glide in quickly; a leftover under half a pixel is finished at once.
        const double g = 1.0 - std::exp(-dtMs / (kNudgeGlideMs / 3.0));
        double nx = nudgeX * g, ny = nudgeY * g;
        if (std::fabs(nudgeX - nx) < 0.5) nx = nudgeX;
        if (std::fabs(nudgeY - ny) < 0.5) ny = nudgeY;
        nudgeX -= nx; nudgeY -= ny;
        dx = (nx + mx) / level;
        dy = (ny + my) / level;
    }
};
}  // namespace wind
