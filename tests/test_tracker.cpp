#include "doctest.h"
#include "../src/tracker.h"
using namespace wind;

TEST_CASE("free mode follows the OS cursor when it moves") {
    Tracker t(1920, 1080, 1.0);
    t.update(100, 100, 0, 0);
    CHECK(t.centerX() == doctest::Approx(100));
    CHECK(t.centerY() == doctest::Approx(100));
    t.update(300, 200, 0, 0);
    CHECK(t.centerX() == doctest::Approx(300));
    CHECK(t.centerY() == doctest::Approx(200));
}
TEST_CASE("locked mode integrates raw deltas when cursor is frozen") {
    Tracker t(1920, 1080, 1.0);
    t.update(960, 540, 0, 0);          // establish position
    t.update(960, 540, 5, -3);         // cursor frozen, raw movement arrives
    CHECK(t.centerX() == doctest::Approx(965));
    CHECK(t.centerY() == doctest::Approx(537));
}
TEST_CASE("sensitivity scales locked-mode panning") {
    Tracker t(1920, 1080, 2.0);
    t.update(960, 540, 0, 0);
    t.update(960, 540, 10, 0);
    CHECK(t.centerX() == doctest::Approx(980)); // 10 * 2.0
}
TEST_CASE("returns to free mode and snaps to cursor when it moves again") {
    Tracker t(1920, 1080, 1.0);
    t.update(960, 540, 0, 0);
    t.update(960, 540, 50, 0);          // locked -> 1010
    t.update(400, 400, 0, 0);           // cursor moved -> free, snap
    CHECK(t.centerX() == doctest::Approx(400));
    CHECK(t.centerY() == doctest::Approx(400));
}
TEST_CASE("holds still when neither cursor nor raw move") {
    Tracker t(1920, 1080, 1.0);
    t.update(700, 700, 0, 0);
    t.update(700, 700, 0, 0);
    CHECK(t.centerX() == doctest::Approx(700));
}
TEST_CASE("clamps center to screen bounds in locked mode") {
    Tracker t(1920, 1080, 1.0);
    t.update(10, 10, 0, 0);
    t.update(10, 10, -1000, -1000);
    CHECK(t.centerX() == doctest::Approx(0));
    CHECK(t.centerY() == doctest::Approx(0));
}
TEST_CASE("recenter snaps to screen center") {
    Tracker t(1920, 1080, 1.0);
    t.update(100, 100, 0, 0);
    t.recenter();
    CHECK(t.centerX() == doctest::Approx(960));
    CHECK(t.centerY() == doctest::Approx(540));
}
