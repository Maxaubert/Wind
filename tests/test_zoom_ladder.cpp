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
        CHECK(std::fabs(z - want) <= want * 0.00501);
        if (z != want) {   // a snap is always clean at least at the pointer
            const bool area = SmoothRoundingErrorArea(z, 3840, 1920.0) < SmoothLadderTolerance(z) &&
                              SmoothRoundingErrorArea(z, 2160, 1080.0) < SmoothLadderTolerance(z);
            const bool ptr = std::fabs(SmoothRoundingError(z, 3840, 1920.0)) < 1.0 &&
                             std::fabs(SmoothRoundingError(z, 2160, 1080.0)) < 1.0;
            CHECK((area || ptr));
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

TEST_CASE("the area error covers more than the pointer: clean at the centre is not enough") {
    // Some levels are clean at the pointer (centre) yet move content at the quarter lines by over 1 px.
    int centreOnly = 0;
    for (double z = 2.0; z < 30.0; z += 0.001) {
        const bool atPtr = std::fabs(SmoothRoundingError(z, 3840, 1920.0)) < 1.0;
        const bool inArea = SmoothRoundingErrorArea(z, 3840, 1920.0) < 1.0;
        if (atPtr && !inArea) ++centreOnly;
    }
    CHECK(centreOnly > 0);
    CHECK(SmoothLadderTolerance(5.0) == 1.0);
    CHECK(SmoothLadderTolerance(12.0) == 1.0);
    CHECK(SmoothLadderTolerance(20.0) == doctest::Approx(1.4));
}
