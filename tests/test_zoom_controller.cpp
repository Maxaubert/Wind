#include "doctest.h"
#include "../src/zoom_controller.h"
#include <cmath>
using namespace wind;

TEST_CASE("ResolveDirection maps physical button state") {
    CHECK(ResolveDirection(false, false) == ZoomDir::None);
    CHECK(ResolveDirection(true,  false) == ZoomDir::In);
    CHECK(ResolveDirection(false, true ) == ZoomDir::Out);
    CHECK(ResolveDirection(true,  true ) == ZoomDir::None); // both held = freeze
}
TEST_CASE("ramps in to 8x in ~1.2s at the default rate") {
    ZoomController z(1.0, 8.0);
    z.setDirection(ZoomDir::In);
    z.tick(1.2);                                 // 2.5 doublings/s * 1.2s = 3 doublings = 8x
    CHECK(z.level() == doctest::Approx(8.0));
}
TEST_CASE("ramps out to min") {
    ZoomController z(1.0, 8.0);
    z.setDirection(ZoomDir::In);  z.tick(1.2);   // at 8.0
    z.setDirection(ZoomDir::Out); z.tick(1.2);
    CHECK(z.level() == doctest::Approx(1.0));
}
TEST_CASE("half the time gives multiplicative midpoint") {
    ZoomController z(1.0, 8.0);
    z.setDirection(ZoomDir::In);
    z.tick(0.6);                                 // 1.5 doublings = 2^1.5
    CHECK(z.level() == doctest::Approx(2.8284).epsilon(0.001));
}
TEST_CASE("zoom speed is independent of maxLevel") {
    // Same hold, two different maxLevels: the level reached is identical until the ceiling clamps.
    ZoomController lo(1.0, 8.0);
    ZoomController hi(1.0, 20.0);
    lo.setDirection(ZoomDir::In); hi.setDirection(ZoomDir::In);
    lo.tick(0.6); hi.tick(0.6);                  // 1.5 doublings, well under either ceiling
    CHECK(lo.level() == doctest::Approx(hi.level()));   // raising maxLevel did NOT speed it up
}
TEST_CASE("freezes when direction is None") {
    ZoomController z(1.0, 8.0);
    z.setDirection(ZoomDir::In); z.tick(0.3);
    double held = z.level();
    z.setDirection(ZoomDir::None); z.tick(5.0);
    CHECK(z.level() == doctest::Approx(held));
}
TEST_CASE("clamps and never exceeds bounds with many small ticks") {
    ZoomController z(1.0, 8.0);
    z.setDirection(ZoomDir::In);
    for (int i = 0; i < 1000; ++i) z.tick(0.01);
    CHECK(z.level() == doctest::Approx(8.0));
}
TEST_CASE("reset returns to 1.0 and None") {
    ZoomController z(1.0, 8.0);
    z.setDirection(ZoomDir::In); z.tick(0.5);
    z.reset();
    CHECK(z.level() == doctest::Approx(1.0));
    CHECK(z.direction() == ZoomDir::None);
}
TEST_CASE("zoomInSpeed multiplies the in rate") {
    ZoomController z(1.0, 8.0);
    z.setProfile(2.0, 1.0, false, 3.0, 0.6);   // 2x in-speed
    z.setDirection(ZoomDir::In);
    z.tick(0.6);                                // 2x for 0.6s == 1x for 1.2s == reaches 8x
    CHECK(z.level() == doctest::Approx(8.0));
}
TEST_CASE("zoomOutSpeed multiplies the out rate") {
    ZoomController z(1.0, 8.0);
    z.setDirection(ZoomDir::In); z.tick(1.2);  // at 8.0 (default profile)
    z.setProfile(1.0, 2.0, false, 3.0, 0.6);   // 2x out-speed
    z.setDirection(ZoomDir::Out); z.tick(0.6); // 2x for 0.6s == back down to min
    CHECK(z.level() == doctest::Approx(1.0));
}
TEST_CASE("smooth zoom-in never exceeds linear (soft start, capped at linear)") {
    ZoomController lin(1.0, 1e9);  // huge ceiling so neither clamps over the whole test
    ZoomController sm (1.0, 1e9);
    lin.setProfile(1.0, 1.0, false, 3.0, 0.2);
    sm .setProfile(1.0, 1.0, true,  3.0, 0.2);
    lin.setDirection(ZoomDir::In); sm.setDirection(ZoomDir::In);
    lin.tick(0.01); sm.tick(0.01);          // first tick: smooth starts SLOWER than linear
    CHECK(sm.level() < lin.level());
    for (int i = 0; i < 200; ++i) { lin.tick(0.01); sm.tick(0.01); }  // long hold, well past the ramp
    CHECK(sm.level() < lin.level());        // eased in below linear, so it never gets ahead
}
TEST_CASE("smooth zoom plateaus at the linear rate (does not exceed it)") {
    ZoomController sm (1.0, 1e9);  sm .setProfile(1.0, 1.0, true,  3.0, 0.1);
    ZoomController lin(1.0, 1e9);  lin.setProfile(1.0, 1.0, false, 3.0, 0.1);
    sm.setDirection(ZoomDir::In);  lin.setDirection(ZoomDir::In);
    for (int i = 0; i < 5; ++i) { sm.tick(0.05); lin.tick(0.05); }  // sm past ramp -> linear rate
    double smBefore = sm.level(), linBefore = lin.level();
    sm.tick(0.1); lin.tick(0.1);
    double smFactor = sm.level() / smBefore, linFactor = lin.level() / linBefore;
    CHECK(smFactor == doctest::Approx(linFactor));   // at the plateau, smooth grows at the linear rate
}
TEST_CASE("releasing resets the smooth ramp so the next in starts slow") {
    ZoomController z(1.0, 1e9);
    z.setProfile(1.0, 1.0, true, 4.0, 0.1);
    z.setDirection(ZoomDir::In);
    for (int i = 0; i < 5; ++i) z.tick(0.05);   // warmed to plateau (fast = linear rate)
    double warmBefore = z.level();
    z.tick(0.02); double warmGain = z.level() - warmBefore;   // a fast (plateau) increment
    z.setDirection(ZoomDir::None); z.tick(0.1); // release -> heldIn resets to 0
    z.setDirection(ZoomDir::In);
    double freshBefore = z.level();
    z.tick(0.02); double freshGain = z.level() - freshBefore;  // should be a slow (eased) increment
    CHECK(freshGain < warmGain);
}
TEST_CASE("zoom-out ignores smooth acceleration") {
    ZoomController z(1.0, 8.0);
    z.setDirection(ZoomDir::In); z.tick(1.2);   // at 8.0
    z.setProfile(1.0, 1.0, true, 5.0, 0.1);     // smooth on, big ease-in depth
    z.setDirection(ZoomDir::Out); z.tick(0.6);  // out is plain 1x rate (no accel) -> 1.5 doublings down
    CHECK(z.level() == doctest::Approx(8.0 / std::pow(2.0, 1.5)));  // == 2.8284, the linear out result
}
TEST_CASE("smooth-zoom guards: accel<1 and ramp=0 don't break the curve") {
    ZoomController a(1.0, 8.0);
    a.setProfile(1.0, 1.0, true, 0.5, 0.0);     // accel<1 and ramp=0
    a.setDirection(ZoomDir::In); a.tick(0.6);
    CHECK(a.level() == doctest::Approx(2.8284).epsilon(0.001));  // accel<1 ignored -> linear midpoint
    ZoomController b(1.0, 1e9);
    b.setProfile(1.0, 1.0, true, 2.0, 0.0);     // ramp=0 -> instant linear, must not divide by zero
    b.setDirection(ZoomDir::In); b.tick(0.01);
    CHECK(b.level() > 1.0);                      // finite, advanced (no NaN/inf)
}
TEST_CASE("setLevel snaps and clamps to bounds") {
    ZoomController z(1.0, 8.0);
    z.setLevel(4.0);
    CHECK(z.level() == doctest::Approx(4.0));
    z.setLevel(100.0);                 // above max -> clamps to 8.0
    CHECK(z.level() == doctest::Approx(8.0));
    z.setLevel(0.5);                   // below min -> clamps to 1.0
    CHECK(z.level() == doctest::Approx(1.0));
}
