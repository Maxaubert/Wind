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
TEST_CASE("ramps in to max over full-range seconds") {
    ZoomController z(1.0, 8.0, 1.2);
    z.setDirection(ZoomDir::In);
    z.tick(1.2);
    CHECK(z.level() == doctest::Approx(8.0));
}
TEST_CASE("ramps out to min") {
    ZoomController z(1.0, 8.0, 1.2);
    z.setDirection(ZoomDir::In);  z.tick(1.2);   // at 8.0
    z.setDirection(ZoomDir::Out); z.tick(1.2);
    CHECK(z.level() == doctest::Approx(1.0));
}
TEST_CASE("half the time gives multiplicative midpoint") {
    ZoomController z(1.0, 8.0, 1.2);
    z.setDirection(ZoomDir::In);
    z.tick(0.6);
    CHECK(z.level() == doctest::Approx(2.8284).epsilon(0.001));
}
TEST_CASE("freezes when direction is None") {
    ZoomController z(1.0, 8.0, 1.2);
    z.setDirection(ZoomDir::In); z.tick(0.3);
    double held = z.level();
    z.setDirection(ZoomDir::None); z.tick(5.0);
    CHECK(z.level() == doctest::Approx(held));
}
TEST_CASE("clamps and never exceeds bounds with many small ticks") {
    ZoomController z(1.0, 8.0, 1.2);
    z.setDirection(ZoomDir::In);
    for (int i = 0; i < 1000; ++i) z.tick(0.01);
    CHECK(z.level() == doctest::Approx(8.0));
}
TEST_CASE("reset returns to 1.0 and None") {
    ZoomController z(1.0, 8.0, 1.2);
    z.setDirection(ZoomDir::In); z.tick(0.5);
    z.reset();
    CHECK(z.level() == doctest::Approx(1.0));
    CHECK(z.direction() == ZoomDir::None);
}
TEST_CASE("zoomInSpeed multiplies the in rate") {
    ZoomController z(1.0, 8.0, 1.2);
    z.setProfile(2.0, 1.0, false, 3.0, 0.6);   // 2x in-speed
    z.setDirection(ZoomDir::In);
    z.tick(0.6);                                // 2x for 0.6s == 1x for 1.2s == full range
    CHECK(z.level() == doctest::Approx(8.0));
}
TEST_CASE("zoomOutSpeed multiplies the out rate") {
    ZoomController z(1.0, 8.0, 1.2);
    z.setDirection(ZoomDir::In); z.tick(1.2);  // at 8.0 (default profile)
    z.setProfile(1.0, 2.0, false, 3.0, 0.6);   // 2x out-speed
    z.setDirection(ZoomDir::Out); z.tick(0.6); // 2x for 0.6s == full range back down
    CHECK(z.level() == doctest::Approx(1.0));
}
TEST_CASE("smooth zoom-in never exceeds linear (soft start, capped at linear)") {
    ZoomController lin(1.0, 8.0, 100.0);  // slow base so neither clamps over the whole test
    ZoomController sm (1.0, 8.0, 100.0);
    lin.setProfile(1.0, 1.0, false, 3.0, 0.2);
    sm .setProfile(1.0, 1.0, true,  3.0, 0.2);
    lin.setDirection(ZoomDir::In); sm.setDirection(ZoomDir::In);
    lin.tick(0.01); sm.tick(0.01);          // first tick: smooth starts SLOWER than linear
    CHECK(sm.level() < lin.level());
    for (int i = 0; i < 200; ++i) { lin.tick(0.01); sm.tick(0.01); }  // long hold, well past the ramp
    CHECK(sm.level() < lin.level());        // eased in below linear, so it never gets ahead
}
TEST_CASE("smooth zoom plateaus at the linear rate (does not exceed it)") {
    ZoomController z(1.0, 8.0, 100.0);          // slow base so it won't clamp
    z.setProfile(1.0, 1.0, true, 3.0, 0.1);     // ease-in depth 3, ramp 0.1s
    z.setDirection(ZoomDir::In);
    for (int i = 0; i < 5; ++i) z.tick(0.05);   // heldIn 0.25s, well past ramp -> at linear rate
    double l1 = z.level();
    z.tick(0.1);
    double l2 = z.level();
    // At the plateau the per-tick factor equals the LINEAR factor pow(R, dt*inSpeed/T) (accelMult==1).
    CHECK(l2 == doctest::Approx(l1 * std::pow(8.0, 0.1 * 1.0 / 100.0)));
}
TEST_CASE("releasing resets the smooth ramp so the next in starts slow") {
    ZoomController z(1.0, 8.0, 100.0);
    z.setProfile(1.0, 1.0, true, 4.0, 0.1);
    z.setDirection(ZoomDir::In);
    for (int i = 0; i < 5; ++i) z.tick(0.05);   // warmed to plateau (fast)
    double warmBefore = z.level();
    z.tick(0.02); double warmGain = z.level() - warmBefore;   // a fast (plateau) increment
    z.setDirection(ZoomDir::None); z.tick(0.1); // release -> heldIn resets to 0
    z.setDirection(ZoomDir::In);
    double freshBefore = z.level();
    z.tick(0.02); double freshGain = z.level() - freshBefore;  // should be a slow (near-base) increment
    CHECK(freshGain < warmGain);
}
TEST_CASE("zoom-out ignores smooth acceleration") {
    ZoomController z(1.0, 8.0, 1.2);
    z.setDirection(ZoomDir::In); z.tick(1.2);   // at 8.0
    z.setProfile(1.0, 1.0, true, 5.0, 0.1);     // smooth on, big accel
    z.setDirection(ZoomDir::Out); z.tick(0.6);  // out should be plain 1x rate (no accel)
    CHECK(z.level() == doctest::Approx(8.0 / std::pow(8.0, 0.5)));  // == 2.8284, the linear out result
}
TEST_CASE("smooth-zoom guards: accel<1 and ramp=0 don't break the curve") {
    ZoomController a(1.0, 8.0, 1.2);
    a.setProfile(1.0, 1.0, true, 0.5, 0.0);     // accel<1 and ramp=0
    a.setDirection(ZoomDir::In); a.tick(0.6);
    CHECK(a.level() == doctest::Approx(2.8284).epsilon(0.001));  // accel<1 ignored -> linear midpoint
    ZoomController b(1.0, 1e9, 1.2);
    b.setProfile(1.0, 1.0, true, 2.0, 0.0);     // ramp=0 -> instant top, must not divide by zero
    b.setDirection(ZoomDir::In); b.tick(0.01);
    CHECK(b.level() > 1.0);                      // finite, advanced (no NaN/inf)
}
