#include "doctest.h"
#include "../src/view_glide.h"
using namespace wind;

TEST_CASE("glide is time-based: two 7 ms steps equal one 14 ms step") {
    double a = GlideToward(GlideToward(0, 100, 7, 150), 100, 7, 150);
    double b = GlideToward(0, 100, 14, 150);
    CHECK(a == doctest::Approx(b).epsilon(1e-9));
}
TEST_CASE("glide covers 95% of the distance in glideMs and never overshoots") {
    double v = 0; for (int i = 0; i < 150; ++i) v = GlideToward(v, 1000, 1, 150);
    CHECK(v == doctest::Approx(950).epsilon(0.01));
    CHECK(v <= 1000);
    CHECK(GlideToward(0, 1000, 0, 150) == 0);          // no time, no motion
    CHECK(GlideToward(0, 1000, 5, 0) == 1000);         // glideMs 0 means snap
}
TEST_CASE("retargeting mid-glide never moves backwards") {
    double v = 0; v = GlideToward(v, 100, 30, 150);
    double w = GlideToward(v, 110, 30, 150);
    CHECK(w > v);
}
TEST_CASE("centred: the target is the rect centre, clamped to the monitor") {
    double cx, cy;
    REQUIRE(TrackTargetCenter({1000, 500, 1002, 520}, 0, 0, 3, 3840, 2160, 0, 15, cx, cy));
    CHECK(cx == doctest::Approx(1001)); CHECK(cy == doctest::Approx(510));
}
TEST_CASE("degenerate and off-monitor rects are rejected") {
    double cx = 7, cy = 7;
    CHECK_FALSE(TrackTargetCenter({0, 0, 0, 0}, 0, 0, 3, 3840, 2160, 0, 15, cx, cy));
    CHECK_FALSE(TrackTargetCenter({500, 500, 400, 600}, 0, 0, 3, 3840, 2160, 0, 15, cx, cy));
    CHECK_FALSE(TrackTargetCenter({-900, 100, -880, 120}, 0, 0, 3, 3840, 2160, 0, 15, cx, cy));
    CHECK_FALSE(TrackTargetCenter({4000, 100, 4010, 120}, 0, 0, 3, 3840, 2160, 0, 15, cx, cy));
    CHECK(cx == 7); CHECK(cy == 7);
}
TEST_CASE("within edges: a rect already inside the margin does not move the view") {
    // level 4 on 3840x2160: view 960x540 around (1920,1080) -> x 1440..2400, y 810..1350
    double cx, cy;
    REQUIRE(TrackTargetCenter({1900, 1000, 1902, 1020}, 1920, 1080, 4, 3840, 2160, 1, 15, cx, cy));
    CHECK(cx == doctest::Approx(1920)); CHECK(cy == doctest::Approx(1080));
}
TEST_CASE("within edges: a rect past the right margin moves the view just enough") {
    // right margin edge = 2400 - 0.15*960 = 2256; rect right 2300 -> shift 44
    double cx, cy;
    REQUIRE(TrackTargetCenter({2298, 1000, 2300, 1020}, 1920, 1080, 4, 3840, 2160, 1, 15, cx, cy));
    CHECK(cx == doctest::Approx(1964)); CHECK(cy == doctest::Approx(1080));
}
TEST_CASE("oversized rect aligns top-left") {
    // a 2000 px wide rect in a 960 px view: its left edge goes to the left margin
    // (1000 - 144 + 480 = 1336, inside the monitor clamp band 480..3360)
    double cx, cy;
    REQUIRE(TrackTargetCenter({1000, 1000, 3000, 1020}, 1920, 1080, 4, 3840, 2160, 1, 15, cx, cy));
    CHECK(cx == doctest::Approx(1000 - 0.15 * 960 + 480));
}

TEST_CASE("spring: settles ~95% of a step in glideMs, never overshoots, same at any tick rate") {
    const double dts[] = { 4.0, 6.94, 16.7 };
    for (double dt : dts) {
        double x = 0, v = 0, peak = 0;
        for (double t = 0; t < 150.0 - 1e-9; t += dt) { x = SpringToward(x, 100, v, dt, 150); if (x > peak) peak = x; }
        CHECK(x > 90); CHECK(x < 99);
        for (int i = 0; i < 400; ++i) { x = SpringToward(x, 100, v, dt, 150); if (x > peak) peak = x; }
        CHECK(peak <= 100.0 + 1e-6);
        CHECK(x == doctest::Approx(100).epsilon(1e-3));
    }
}
TEST_CASE("spring: steady typing becomes a steady glide (velocity carries across retargets)") {
    double x = 0, v = 0, target = 0;
    for (int k = 0; k < 20; ++k) {           // a keystroke every 120 ms, 10 px each
        target += 10;
        for (int i = 0; i < 17; ++i) x = SpringToward(x, target, v, 7, 150);
    }
    INFO("v=" << v << " lag=" << (target - x));
    CHECK(v > 20);                            // still moving between keys: not stop-start
    CHECK(target - x < 25);                   // and within ~2.5 characters of the caret
}
TEST_CASE("spring: glideMs 0 snaps and clears velocity") {
    double v = 5; CHECK(SpringToward(3, 9, v, 7, 0) == 9); CHECK(v == 0);
}
