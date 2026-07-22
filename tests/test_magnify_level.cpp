#include "doctest.h"
#include "../src/magnify_level.h"

using namespace wind;

TEST_CASE("MagnifyTargetPct: smooth level to integer percent") {
    CHECK(MagnifyTargetPct(1.0) == 100);
    CHECK(MagnifyTargetPct(1.37) == 137);      // arbitrary percents apply exactly (measured)
    CHECK(MagnifyTargetPct(2.0) == 200);
    CHECK(MagnifyTargetPct(2.999) == 300);     // nearest percent
    CHECK(MagnifyTargetPct(1.004) == 100);     // 1.005 is not exactly representable (100.4999...),
    CHECK(MagnifyTargetPct(1.006) == 101);     // so test the rounding boundary with clean values
}

TEST_CASE("MagnifyTargetPct: mandatory clamps") {
    // Below 1x clamps up (Magnifier has no sub-1x).
    CHECK(MagnifyTargetPct(0.5) == 100);
    // Above 1600% Magnifier IGNORES the write (measured), so the clamp must happen here.
    CHECK(MagnifyTargetPct(16.0) == 1600);
    CHECK(MagnifyTargetPct(20.0) == 1600);
    CHECK(MagnifyTargetPct(1e9) == 1600);
}

// Screen position of desktop point p under transform (lvl, off).
static double T(double p, double off, double lvl) { return (p - off) * lvl; }

TEST_CASE("MagnifyAnchorOffset: the cursor's desktop point keeps its screen position") {
    const int sw = 3840, sh = 2160;
    // Zooming 2.0 -> 2.5 with the cursor at an arbitrary desktop point, from an arbitrary offset.
    double px = 900, py = 700, lvl0 = 2.0, ox0 = 400, oy0 = 300, lvl1 = 2.5;
    MagnifyOffset o = MagnifyAnchorOffset(px, py, lvl0, ox0, oy0, lvl1, sw, sh);
    CHECK(T(px, o.x, lvl1) == doctest::Approx(T(px, ox0, lvl0)));
    CHECK(T(py, o.y, lvl1) == doctest::Approx(T(py, oy0, lvl0)));
}

TEST_CASE("MagnifyAnchorOffset: first tick from rest equals the classic fixed-point offset") {
    const int sw = 3840, sh = 2160;
    // From identity (lvl 1, off 0) the anchor solution reduces to off = p * (1 - 1/lvl).
    double px = 1234, py = 567, lvl1 = 3.0;
    MagnifyOffset o = MagnifyAnchorOffset(px, py, 1.0, 0, 0, lvl1, sw, sh);
    CHECK(o.x == doctest::Approx(px * (1.0 - 1.0 / lvl1)));
    CHECK(o.y == doctest::Approx(py * (1.0 - 1.0 / lvl1)));
}

TEST_CASE("MagnifyAnchorOffset: zooming out to 1x lands exactly at the origin") {
    MagnifyOffset o = MagnifyAnchorOffset(500, 400, 4.0, 375, 300, 1.0, 3840, 2160);
    CHECK(o.x == doctest::Approx(0.0));
    CHECK(o.y == doctest::Approx(0.0));
}

TEST_CASE("MagnifyAnchorOffset: clamps to the legal source range at screen corners") {
    const int sw = 3840, sh = 2160;
    // Cursor at the bottom-right corner: unclamped anchor would run past the max source origin.
    MagnifyOffset o = MagnifyAnchorOffset(sw, sh, 1.0, 0, 0, 2.0, sw, sh);
    CHECK(o.x == doctest::Approx(sw / 2.0));   // max = sw - sw/2
    CHECK(o.y == doctest::Approx(sh / 2.0));
    // Top-left corner clamps to 0.
    MagnifyOffset z = MagnifyAnchorOffset(0, 0, 1.0, 0, 0, 2.0, sw, sh);
    CHECK(z.x == doctest::Approx(0.0));
    CHECK(z.y == doctest::Approx(0.0));
}
