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

TEST_CASE("a tap moves exactly one nudge, 1/8 of the screen, at any zoom level (#287)") {
    const double levels[2] = { 2.0, 8.0 };
    for (double level : levels) {
        KeyPan p; double sx = 0, sy = 0;
        const bool left[4] = { true, false, false, false };
        Run(p, left, 7, level, 1.0, sx, sy);           // one tick down
        Run(p, kNone, 1000, level, 1.0, sx, sy);        // released
        CHECK(sx == doctest::Approx(-3840.0 / 8 / level).epsilon(0.01));
        CHECK(sy == doctest::Approx(0.0));
        CHECK_FALSE(p.active());
    }
}
TEST_CASE("holding pans continuously after 250 ms at speed x half a screen per second") {
    KeyPan p; double sx = 0, sy = 0;
    const bool right[4] = { false, true, false, false };
    Run(p, right, 2000, 2.0, 1.0, sx, sy);
    const double expect = (3840.0 / 8 + (2.0 - 0.25) * 0.5 * 3840.0) / 2.0;
    CHECK(sx > expect * 0.9); CHECK(sx < expect * 1.05);
    Run(p, kNone, 600, 2.0, 1.0, sx, sy);
    CHECK_FALSE(p.active());                            // glides to rest after release
}
TEST_CASE("speed scales the continuous part; the nudge is unchanged") {
    const bool down[4] = { false, false, false, true };
    KeyPan a, b; double ax = 0, ay = 0, bx = 0, by = 0;
    Run(a, down, 2000, 4.0, 1.0, ax, ay);
    Run(b, down, 2000, 4.0, 2.0, bx, by);
    const double nudge = 2160.0 / 8 / 4.0;
    CHECK((by - nudge) == doctest::Approx(2.0 * (ay - nudge)).epsilon(0.05));
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
