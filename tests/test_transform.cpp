#include "doctest.h"
#include "../src/transform.h"

TEST_CASE("ComputeOffsetF centers sub-pixel at 2x") {
    wind::OffsetF o = wind::ComputeOffsetF(960.25, 540.0, 2.0, 1920, 1080);
    CHECK(o.x == doctest::Approx(480.25));
    CHECK(o.y == doctest::Approx(270.0));
}
TEST_CASE("ComputeOffsetF clamps to top-left edge (float)") {
    wind::OffsetF o = wind::ComputeOffsetF(0.0, 0.0, 2.0, 1920, 1080);
    CHECK(o.x == doctest::Approx(0.0));
    CHECK(o.y == doctest::Approx(0.0));
}
TEST_CASE("ComputeOffsetF clamps to bottom-right edge (float)") {
    wind::OffsetF o = wind::ComputeOffsetF(1e9, 1e9, 4.0, 1920, 1080);
    CHECK(o.x == doctest::Approx(1920.0 - 1920.0 / 4.0));   // 1440
    CHECK(o.y == doctest::Approx(1080.0 - 1080.0 / 4.0));   // 810
}
TEST_CASE("ComputeOffsetF at 1x is origin") {
    wind::OffsetF o = wind::ComputeOffsetF(500.0, 500.0, 1.0, 1920, 1080);
    CHECK(o.x == doctest::Approx(0.0));
    CHECK(o.y == doctest::Approx(0.0));
}
TEST_CASE("ComputeOffsetF clamps level below 1.0 to 1x (whole screen, origin)") {
    // A level < 1.0 (e.g. from a bad/clamped config) must behave exactly like 1x, not invert.
    wind::OffsetF a = wind::ComputeOffsetF(960.0, 540.0, 0.5, 1920, 1080);
    wind::OffsetF b = wind::ComputeOffsetF(960.0, 540.0, 1.0, 1920, 1080);
    CHECK(a.x == doctest::Approx(b.x));
    CHECK(a.y == doctest::Approx(b.y));
    CHECK(a.x == doctest::Approx(0.0));
    CHECK(a.y == doctest::Approx(0.0));
}
