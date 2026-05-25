#include "doctest.h"
#include "../src/cursor_mapper.h"

using wind::CursorMapper;

TEST_CASE("centered: cursor sits at screen center, no raw movement") {
    CursorMapper m(1920, 1080, 1.0);
    m.reset(960, 540);
    auto r = m.update(0, 0, 2.0);
    CHECK(r.srcLeft == doctest::Approx(480.0));   // 960 - (1920/2)/2
    CHECK(r.srcTop  == doctest::Approx(270.0));
    CHECK(r.cursorScreenX == doctest::Approx(960.0));  // dead center
    CHECK(r.cursorScreenY == doctest::Approx(540.0));
    CHECK(r.clickDesktopX == 960);
    CHECK(r.clickDesktopY == 540);
}

TEST_CASE("centered: raw movement pans the world at desktop speed, cursor stays centered") {
    CursorMapper m(1920, 1080, 1.0);     // sensitivity 1.0
    m.reset(960, 540);
    auto r = m.update(20, 0, 2.0);       // 20 raw * 1.0 = +20 desktop px (zoom-independent)
    CHECK(m.centerX() == doctest::Approx(980.0));
    CHECK(r.srcLeft == doctest::Approx(500.0));        // 980 - 480
    CHECK(r.cursorScreenX == doctest::Approx(960.0));  // still centered
    CHECK(r.clickDesktopX == 980);                     // click point tracks lens center
}

TEST_CASE("edge: cursor shifts off-center when the view clamps at the desktop edge") {
    CursorMapper m(1920, 1080, 1.0);
    m.reset(10, 540);                    // near the left edge
    auto r = m.update(0, 0, 4.0);        // viewW = 480, srcLeft clamps to 0
    CHECK(r.srcLeft == doctest::Approx(0.0));
    CHECK(r.cursorScreenX == doctest::Approx(40.0));   // (10 - 0) * 4
    CHECK(r.clickDesktopX == 10);
}

TEST_CASE("lens center clamps to the desktop bounds") {
    CursorMapper m(1920, 1080, 1.0);
    m.reset(0, 0);
    m.update(-500, -500, 2.0);           // pushes past the top-left
    CHECK(m.centerX() == doctest::Approx(0.0));
    CHECK(m.centerY() == doctest::Approx(0.0));
    m.reset(1920, 1080);
    m.update(500, 500, 2.0);             // pushes past the bottom-right
    CHECK(m.centerX() == doctest::Approx(1920.0));
    CHECK(m.centerY() == doctest::Approx(1080.0));
}

TEST_CASE("sensitivity scales lens movement; zoom level does NOT change desktop speed") {
    CursorMapper slow(1920, 1080, 0.5);
    slow.reset(960, 540);
    slow.update(40, 0, 2.0);             // 40 * 0.5 = +20
    CHECK(slow.centerX() == doctest::Approx(980.0));

    // Same raw delta + sensitivity at a higher zoom moves the lens the SAME desktop amount
    // (the world just scrolls faster on screen) - the fix for "cursor too slow when zoomed".
    CursorMapper a(1920, 1080, 1.0);  a.reset(960, 540);  a.update(40, 0, 2.0);
    CursorMapper b(1920, 1080, 1.0);  b.reset(960, 540);  b.update(40, 0, 8.0);
    CHECK(a.centerX() == doctest::Approx(1000.0));
    CHECK(b.centerX() == doctest::Approx(1000.0));     // zoom-independent
}

TEST_CASE("smoothing eases the rendered center toward the target (light inertia)") {
    CursorMapper m(1920, 1080, 1.0, 0.5);    // alpha = 0.5
    m.reset(960, 540);
    m.update(40, 0, 2.0);    // target 1000; rendered = 960 + (1000-960)*0.5 = 980
    CHECK(m.centerX() == doctest::Approx(980.0));
    m.update(0, 0, 2.0);     // target still 1000; rendered = 980 + (1000-980)*0.5 = 990
    CHECK(m.centerX() == doctest::Approx(990.0));
}

TEST_CASE("smoothing 0 snaps instantly (no inertia)") {
    CursorMapper m(1920, 1080, 1.0, 0.0);
    m.reset(960, 540);
    m.update(40, 0, 2.0);
    CHECK(m.centerX() == doctest::Approx(1000.0));   // straight to target
}

TEST_CASE("reset overrides the accumulated center") {
    CursorMapper m(1920, 1080, 1.0);
    m.reset(100, 100);
    m.update(50, 50, 2.0);
    m.reset(800, 400);
    CHECK(m.centerX() == doctest::Approx(800.0));
    CHECK(m.centerY() == doctest::Approx(400.0));
}
