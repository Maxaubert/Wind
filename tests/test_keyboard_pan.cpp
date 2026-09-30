#include <cmath>
#include "doctest.h"
#include "../src/keyboard_pan.h"
using namespace wind;

// Index order: Left, Right, Up, Down.
static void Run(KeyPan& p, const bool held[4], double ms, double level, double speed, double& sx, double& sy) {
    for (double t = 0; t < ms; t += 7) {
        double dx = 0, dy = 0;
        p.step(held, 7, level, 3840, 2160, speed, dx, dy);
        sx += dx; sy += dy;
    }
}
static const bool kNone[4] = { false, false, false, false };

TEST_CASE("a quick tap moves exactly one nudge, 1/8 of the screen, at any zoom level (#287)") {
    const double levels[2] = { 2.0, 8.0 };
    const double tapMs[2] = { 7, 70 };   // a longer tap already moved past a nudge: nothing to top up
    for (double level : levels) for (double ms : tapMs) {
        KeyPan p; double sx = 0, sy = 0;
        const bool left[4] = { true, false, false, false };
        Run(p, left, ms, level, 1.0, sx, sy);
        Run(p, kNone, 1000, level, 1.0, sx, sy);
        CHECK(sx == doctest::Approx(-3840.0 / 8 / level).epsilon(0.01));
        CHECK(sy == doctest::Approx(0.0));
        CHECK_FALSE(p.active());
    }
}
TEST_CASE("a hold starts moving at once and never adds the nudge (no jump)") {
    KeyPan p; double sx = 0, sy = 0;
    const bool right[4] = { false, true, false, false };
    Run(p, right, 7, 2.0, 1.0, sx, sy);
    CHECK(sx > 0);                                       // moving on the first tick
    double prev = sx;
    for (int i = 0; i < 60; ++i) {                       // per-tick steps never exceed full speed
        double dx = 0, dy = 0; p.step(right, 7, 2.0, 3840, 2160, 1.0, dx, dy);
        CHECK(dx <= KeyPan::ZoomRateScale(2.0) * 1.25 * 3840 * 0.007 / 2.0 + 1e-6);
        sx += dx; prev = sx;
    }
    (void)prev;
}
TEST_CASE("holding pans at speed x 1.25 screens per second (scaled at 2x), then glides to rest") {
    KeyPan p; double sx = 0, sy = 0;
    const bool right[4] = { false, true, false, false };
    Run(p, right, 2000, 2.0, 1.0, sx, sy);
    const double full = KeyPan::ZoomRateScale(2.0) * 1.25 * 3840.0 * 2.0 / 2.0;   // 2 s at the 2x rate
    CHECK(sx > full * 0.9); CHECK(sx < full);
    Run(p, kNone, 600, 2.0, 1.0, sx, sy);
    CHECK_FALSE(p.active());
}
TEST_CASE("every direction pans at the same pixel rate and taps the same step (field test)") {
    const bool right[4] = { false, true, false, false }, down[4] = { false, false, false, true };
    KeyPan a, b; double ax = 0, ay = 0, bx = 0, by = 0;
    Run(a, right, 1000, 3.0, 1.0, ax, ay);
    Run(b, down, 1000, 3.0, 1.0, bx, by);
    CHECK(by == doctest::Approx(ax).epsilon(0.001));
    const bool up[4] = { false, false, true, false };
    KeyPan c; double cx = 0, cy = 0;
    Run(c, up, 7, 2.0, 1.0, cx, cy); Run(c, kNone, 1000, 2.0, 1.0, cx, cy);
    CHECK(cy == doctest::Approx(-3840.0 / 8 / 2.0).epsilon(0.01));
}
TEST_CASE("pan speed follows one smooth curve of the zoom, no corners or steps (#305)") {
    CHECK(KeyPan::ZoomRateScale(2.0) == doctest::Approx(std::pow(2.0 / 7.0, 0.6)).epsilon(0.01));   // power law at low zoom
    CHECK(KeyPan::ZoomRateScale(7.5) == doctest::Approx(0.93).epsilon(0.01));
    CHECK(KeyPan::ZoomRateScale(10.0) == doctest::Approx(0.98).epsilon(0.01));
    CHECK(KeyPan::ZoomRateScale(40.0) < 1.0);
    CHECK(KeyPan::ZoomRateScale(40.0) > 0.999);
    // Smooth: sampled every 0.01x from 1x to 30x the curve always rises, and its slope changes by only
    // a tiny amount from one sample to the next (a corner or a step would show as a jump here).
    const double h = 0.01;
    double prevSlope = (KeyPan::ZoomRateScale(1.0 + h) - KeyPan::ZoomRateScale(1.0)) / h;
    for (double l = 1.0 + h; l < 30.0; l += h) {
        const double slope = (KeyPan::ZoomRateScale(l + h) - KeyPan::ZoomRateScale(l)) / h;
        CHECK(slope > 0);
        CHECK(std::fabs(slope - prevSlope) < 0.003);
        prevSlope = slope;
    }
    const bool right[4] = { false, true, false, false };
    KeyPan a, b; double ax = 0, ay = 0, bx = 0, by = 0;
    Run(a, right, 2000, 2.0, 1.0, ax, ay);
    Run(b, right, 2000, 8.0, 1.0, bx, by);
    CHECK((ax * 2.0) / (bx * 8.0) == doctest::Approx(KeyPan::ZoomRateScale(2.0) / KeyPan::ZoomRateScale(8.0)).epsilon(0.01));
}
TEST_CASE("speed scales the pan linearly") {
    const bool down[4] = { false, false, false, true };
    KeyPan a, b; double ax = 0, ay = 0, bx = 0, by = 0;
    Run(a, down, 2000, 4.0, 1.0, ax, ay);
    Run(b, down, 2000, 4.0, 2.0, bx, by);
    CHECK(by == doctest::Approx(2.0 * ay).epsilon(0.02));
}
TEST_CASE("two keys pan diagonally; opposite keys cancel") {
    KeyPan p; double sx = 0, sy = 0;
    const bool upLeft[4] = { true, false, true, false };
    Run(p, upLeft, 1000, 2.0, 1.0, sx, sy);
    CHECK(sx < 0); CHECK(sy < 0);
    KeyPan q; double qx = 0, qy = 0;
    const bool both[4] = { true, true, false, false };
    Run(q, both, 1000, 2.0, 1.0, qx, qy);
    CHECK(qx == doctest::Approx(0.0).epsilon(0.001));
}
TEST_CASE("idle KeyPan is inactive and moves nothing") {
    KeyPan p; double sx = 0, sy = 0;
    CHECK_FALSE(p.active());
    Run(p, kNone, 100, 3.0, 1.0, sx, sy);
    CHECK(sx == 0); CHECK(sy == 0);
}
