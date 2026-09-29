#include "doctest.h"
#include "../src/edge_pan.h"
using namespace wind;

// level 4 on 3840x2160: view 960x540; at (1920,1080) the band is x 1584..2256, y 891..1269.
TEST_CASE("edge pan: a pointer inside the band leaves the view alone") {
    double cx, cy;
    EdgePanCenter(1920, 1080, 2000, 1100, 4, 3840, 2160, 15, -1, -1, cx, cy);
    CHECK(cx == doctest::Approx(1920)); CHECK(cy == doctest::Approx(1080));
}
TEST_CASE("edge pan: past the right band edge the view follows just enough") {
    double cx, cy;
    EdgePanCenter(1920, 1080, 2300, 1080, 4, 3840, 2160, 15, -1, -1, cx, cy);
    CHECK(cx == doctest::Approx(1920 + 44)); CHECK(cy == doctest::Approx(1080));
}
TEST_CASE("edge pan: the view never leaves the monitor") {
    double cx, cy;
    EdgePanCenter(480, 270, 0, 0, 4, 3840, 2160, 15, -1, -1, cx, cy);
    CHECK(cx == doctest::Approx(480)); CHECK(cy == doctest::Approx(270));
}
TEST_CASE("edge pan: the MPO wall bounds the source left edge directly") {
    double cx, cy;   // wall srcLeft <= 2000 -> centre <= 2480
    EdgePanCenter(2400, 1080, 3800, 1080, 4, 3840, 2160, 15, 2000, -1, cx, cy);
    CHECK(cx == doctest::Approx(2480));
}
TEST_CASE("edge clamp: a pointer outside the view is placed just inside the band; inside stays put") {
    double x, y;
    EdgeClampPointer(1920, 1080, 100, 3000, 4, 3840, 2160, 15, x, y);
    CHECK(x == doctest::Approx(1584)); CHECK(y == doctest::Approx(1269));
    EdgeClampPointer(1920, 1080, 2000, 1000, 4, 3840, 2160, 15, x, y);
    CHECK(x == doctest::Approx(2000)); CHECK(y == doctest::Approx(1000));
}
