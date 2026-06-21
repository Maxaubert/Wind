#include "doctest.h"
#include "../src/mouse_ballistics.h"
#include <cmath>

using wind::BallisticsConfig;
using wind::CookMickeyPacket;
using wind::PointerSpeedMultiplier;

TEST_CASE("PointerSpeedMultiplier matches the Windows slider table") {
    CHECK(PointerSpeedMultiplier(10) == doctest::Approx(1.0));   // default
    CHECK(PointerSpeedMultiplier(1)  == doctest::Approx(0.03125));
    CHECK(PointerSpeedMultiplier(20) == doctest::Approx(3.5));
    CHECK(PointerSpeedMultiplier(14) == doctest::Approx(2.0));
    // clamps out of range
    CHECK(PointerSpeedMultiplier(0)   == doctest::Approx(0.03125));
    CHECK(PointerSpeedMultiplier(999) == doctest::Approx(3.5));
}

TEST_CASE("No acceleration: cooked = raw * slider, exactly (1:1 at default)") {
    BallisticsConfig c;            // accelEnabled defaults to false
    c.sliderMult = 1.0;
    double x, y;
    CookMickeyPacket(c, 7, -3, x, y);
    CHECK(x == doctest::Approx(7.0));
    CHECK(y == doctest::Approx(-3.0));

    c.sliderMult = 1.5;
    CookMickeyPacket(c, 4, 0, x, y);
    CHECK(x == doctest::Approx(6.0));
    CHECK(y == doctest::Approx(0.0));
}

TEST_CASE("Zero packet yields zero") {
    BallisticsConfig c; c.accelEnabled = true; c.sliderMult = 1.0;
    double x = 9, y = 9;
    CookMickeyPacket(c, 0, 0, x, y);
    CHECK(x == doctest::Approx(0.0));
    CHECK(y == doctest::Approx(0.0));
}

TEST_CASE("Acceleration: slow movement is ~1:1 with the slider baseline") {
    BallisticsConfig c; c.accelEnabled = true; c.sliderMult = 1.0;
    // A single-mickey packet sits at the bottom of the curve: gain normalized to ~slider (1.0).
    double x, y;
    CookMickeyPacket(c, 1, 0, x, y);
    CHECK(x == doctest::Approx(1.0).epsilon(0.05));   // within 5% of 1:1
}

TEST_CASE("Acceleration: fast movement gains more than slow (monotonic ramp)") {
    BallisticsConfig c; c.accelEnabled = true; c.sliderMult = 1.0;
    auto gain = [&](int counts) {
        double x, y; CookMickeyPacket(c, counts, 0, x, y);
        return x / counts;
    };
    double gSlow = gain(1), gMed = gain(10), gFast = gain(30);
    CHECK(gSlow < gMed);
    CHECK(gMed  < gFast);
    CHECK(gSlow == doctest::Approx(1.0).epsilon(0.05));
    CHECK(gFast > gSlow);   // a fast flick is accelerated above the slow baseline
}

TEST_CASE("Slider scales the whole result up") {
    BallisticsConfig c; c.accelEnabled = true;
    auto slowGain = [&](double mult) {
        c.sliderMult = mult;
        double x, y; CookMickeyPacket(c, 1, 0, x, y);
        return x;   // gain at 1 mickey ~= slider baseline
    };
    CHECK(slowGain(2.0) > slowGain(1.0));
    CHECK(slowGain(2.0) == doctest::Approx(2.0).epsilon(0.10));
}

TEST_CASE("Diagonal magnitude drives the curve (not per-axis)") {
    BallisticsConfig c; c.accelEnabled = true; c.sliderMult = 1.0;
    // Direction is preserved: equal dx,dy -> equal cooked components.
    double x, y;
    CookMickeyPacket(c, 10, 10, x, y);
    CHECK(x == doctest::Approx(y));
    CHECK(x > 10.0);   // accelerated (magnitude ~14)
}
