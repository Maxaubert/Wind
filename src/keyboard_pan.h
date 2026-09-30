#pragma once
// Keyboard panning (issue #287): a tap moves the view one nudge (1/8 of the screen), holding a key
// past 250 ms pans continuously with a short ease-in, and release glides out. Rates are in SCREEN
// space so the feel is the same at every zoom; step() returns desktop-pixel deltas. Pure;
// tests/test_keyboard_pan.cpp. Key index order: Left, Right, Up, Down.
#include <cmath>
namespace wind {
struct KeyPan {
    static constexpr double kNudgeFrac = 0.125;      // a tap: 1/8 of the screen
    static constexpr double kHoldMs = 250.0;         // held longer than this: continuous pan
    static constexpr double kScreensPerSec = 0.5;    // continuous rate at panSpeed 1.0
    static constexpr double kEaseInMs = 150.0, kGlideMs = 120.0;

    double heldMs[4] = { 0, 0, 0, 0 };
    double nudgeX = 0, nudgeY = 0;   // screen px of nudges still to travel
    double vx = 0, vy = 0;           // continuous velocity, screen px/s

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
        static const double kSign[4] = { -1, 1, -1, 1 };
        double tvx = 0, tvy = 0;
        for (int i = 0; i < 4; ++i) {
            const bool horiz = i < 2;
            const double span = horiz ? monW : monH;
            if (!held[i]) { heldMs[i] = 0; continue; }
            if (heldMs[i] == 0) (horiz ? nudgeX : nudgeY) += kSign[i] * kNudgeFrac * span;   // first down
            heldMs[i] += dtMs;
            if (heldMs[i] >= kHoldMs) (horiz ? tvx : tvy) += kSign[i] * speed * kScreensPerSec * span;
        }
        // Velocity chases its target: ~150 ms to full speed, ~120 ms to rest (3 time constants).
        const bool driving = tvx != 0 || tvy != 0;
        const double a = 1.0 - std::exp(-dtMs / ((driving ? kEaseInMs : kGlideMs) / 3.0));
        vx += (tvx - vx) * a;
        vy += (tvy - vy) * a;
        if (tvx == 0 && std::fabs(vx) < 1.0) vx = 0;
        if (tvy == 0 && std::fabs(vy) < 1.0) vy = 0;
        // Nudges glide out over ~120 ms; a leftover under half a pixel is finished at once.
        const double g = 1.0 - std::exp(-dtMs / (kGlideMs / 3.0));
        double nx = nudgeX * g, ny = nudgeY * g;
        if (std::fabs(nudgeX - nx) < 0.5) nx = nudgeX;
        if (std::fabs(nudgeY - ny) < 0.5) ny = nudgeY;
        nudgeX -= nx; nudgeY -= ny;
        dx = (nx + vx * dtMs / 1000.0) / level;
        dy = (ny + vy * dtMs / 1000.0) / level;
    }
};
}  // namespace wind
