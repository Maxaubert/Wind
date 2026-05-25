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

TEST_CASE("centered: raw movement pans the world, cursor stays centered") {
    CursorMapper m(1920, 1080, 1.0);     // sensitivity 1.0
    m.reset(960, 540);
    auto r = m.update(20, 0, 2.0);       // 20 raw * 1.0 / 2.0 level = +10 desktop px
    CHECK(m.centerX() == doctest::Approx(970.0));
    CHECK(r.srcLeft == doctest::Approx(490.0));        // 970 - 480
    CHECK(r.cursorScreenX == doctest::Approx(960.0));  // still centered
    CHECK(r.clickDesktopX == 970);                     // click point tracks lens center
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

TEST_CASE("sensitivity scales lens movement; higher zoom moves the lens less per raw count") {
    CursorMapper slow(1920, 1080, 0.5);
    slow.reset(960, 540);
    slow.update(40, 0, 2.0);             // 40 * 0.5 / 2.0 = +10
    CHECK(slow.centerX() == doctest::Approx(970.0));

    CursorMapper z(1920, 1080, 1.0);
    z.reset(960, 540);
    z.update(40, 0, 8.0);                // 40 * 1.0 / 8.0 = +5
    CHECK(z.centerX() == doctest::Approx(965.0));
}

TEST_CASE("reset overrides the accumulated center") {
    CursorMapper m(1920, 1080, 1.0);
    m.reset(100, 100);
    m.update(50, 50, 2.0);
    m.reset(800, 400);
    CHECK(m.centerX() == doctest::Approx(800.0));
    CHECK(m.centerY() == doctest::Approx(400.0));
}
