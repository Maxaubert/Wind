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
