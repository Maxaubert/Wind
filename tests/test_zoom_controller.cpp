#include "doctest.h"
#include "../src/zoom_controller.h"
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
