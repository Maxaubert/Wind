#include "doctest.h"
#include "../src/zoom_ladder.h"
#include <cmath>

using namespace wind;

TEST_CASE("smooth rounding error: zero where the scratch size and origin are whole") {
    // 3840x2160 at 4x, pointer centred: 3840/4 = 960, origin 1920 - 480 = 1440 (whole).
    CHECK(std::fabs(SmoothRoundingError(4.0, 3840, 1920.0)) < 1e-9);
    CHECK(std::fabs(SmoothRoundingError(4.0, 2160, 1080.0)) < 1e-9);
}

TEST_CASE("smooth rounding error grows at awkward levels") {
    double worst = 0;
    for (double z = 10.0; z < 25.0; z += 0.01) {
        const double e = std::fabs(SmoothRoundingError(z, 3840, 1920.0));
        if (e > worst) worst = e;
    }
    CHECK(worst > 5.0);   // the measured shake class (11-12 px p95 at 10-25x)
}

TEST_CASE("snapped levels are low-error, close to the request, and never reverse a ramp") {
    for (double want = 2.0; want < 25.0; want *= 1.013) {
        const double z = SnapSmoothLevel(want, 1920.0, 1080.0, 3840, 2160);
        CHECK(std::fabs(z - want) <= want * 0.0201);
        if (z != want) {
            CHECK(std::fabs(SmoothRoundingError(z, 3840, 1920.0)) < 1.0);
            CHECK(std::fabs(SmoothRoundingError(z, 2160, 1080.0)) < 1.0);
        }
        const double up = SnapSmoothLevel(want, 1920.0, 1080.0, 3840, 2160, want, +1);
        CHECK(up >= want);
        const double dn = SnapSmoothLevel(want, 1920.0, 1080.0, 3840, 2160, want, -1);
        CHECK(dn <= want);
    }
}

TEST_CASE("1x and degenerate inputs pass through") {
    CHECK(SnapSmoothLevel(1.0, 1920.0, 1080.0, 3840, 2160) == 1.0);
    CHECK(SnapSmoothLevel(3.0, 1920.0, 1080.0, 0, 0) == 3.0);
}
