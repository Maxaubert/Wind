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
        CHECK(std::fabs(z - want) <= want * (SnapWindow(want) + 1e-5));
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

TEST_CASE("the snap window grows with zoom and high zoom still finds pointer-clean levels") {
    CHECK(SnapWindow(5.0) == 0.005);
    CHECK(SnapWindow(12.0) == 0.005);
    CHECK(SnapWindow(30.0) == doctest::Approx(0.012));
    int found = 0, total = 0;
    for (double want = 20.0; want < 31.0; want *= 1.007, ++total) {
        const double z = SnapSmoothLevel(want, 1920.0, 1080.0, 3840, 2160);
        if (std::fabs(SmoothRoundingError(z, 3840, 1920.0)) < 1.0 &&
            std::fabs(SmoothRoundingError(z, 2160, 1080.0)) < 1.0) ++found;
    }
    CHECK(found >= total * 9 / 10);
}

TEST_CASE("a slow (easing) zoom is barely snapped: the snap never exceeds the frame's own motion") {
    for (double want = 3.0; want < 30.0; want *= 1.01) {
        const double step = 0.0003;   // ease-out: 0.03 % this frame
        const double z = SnapSmoothLevel(want, 1920.0, 1080.0, 3840, 2160, 0.0, 0, -1.0, step);
        CHECK(std::fabs(z - want) <= want * (step + 1e-6));
    }
}

TEST_CASE("rounding cells: a tiny step usually stays inside, a long glide leaves") {
    int same = 0, total = 0;
    for (double z = 3.0; z < 20.0; z *= 1.003, ++total)
        if (SameRoundingCell(z, z * 1.00005, 1920.0, 1080.0, 3840, 2160)) ++same;
    CHECK(same > total * 8 / 10);
    CHECK(SameRoundingCell(4.0, 6.0, 1920.0, 1080.0, 3840, 2160) == false);
    CHECK(SameRoundingCell(5.0, 5.0, 1920.0, 1080.0, 3840, 2160) == true);
}

TEST_CASE("the ease-out runs while it moves faster than the clean-level spacing, then stops") {
    CHECK(EaseOutShouldStop(5.0, 5.0 / 1.02) == false);    // 2 % per frame: still gliding
    CHECK(EaseOutShouldStop(5.0, 5.0 / 1.001) == true);    // 0.1 % per frame: the tail, stop
    CHECK(EaseOutShouldStop(25.0, 25.0 / 1.004) == true);  // 0.4 % at 25x (window 1.2 %)
    CHECK(EaseOutShouldStop(1.0, 1.0) == false);
}
