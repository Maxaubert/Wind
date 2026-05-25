#include "doctest.h"
#include "../src/transform.h"

TEST_CASE("centers the view at 2x") {
    wind::Offset o = wind::ComputeOffset(960, 540, 2.0, 1920, 1080);
    CHECK(o.x == 480);
    CHECK(o.y == 270);
}
TEST_CASE("clamps to top-left edge") {
    wind::Offset o = wind::ComputeOffset(0, 0, 2.0, 1920, 1080);
    CHECK(o.x == 0);
    CHECK(o.y == 0);
}
TEST_CASE("clamps to bottom-right edge") {
    wind::Offset o = wind::ComputeOffset(1920, 1080, 2.0, 1920, 1080);
    CHECK(o.x == 960);
    CHECK(o.y == 540);
}
TEST_CASE("level 1.0 always offsets to origin") {
    wind::Offset o = wind::ComputeOffset(500, 500, 1.0, 1920, 1080);
    CHECK(o.x == 0);
    CHECK(o.y == 0);
}
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
