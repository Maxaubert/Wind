#include "doctest.h"
#include "../src/game_cursor.h"

using namespace wind;

TEST_CASE("a game draws its cursor only when a covering app hides the pointer (#443)") {
    CHECK(GameDrawsCursor(true, true, false, false));
    CHECK_FALSE(GameDrawsCursor(true, true, true, false));    // pointer shown: DWM centring as usual
    CHECK_FALSE(GameDrawsCursor(true, false, false, false));  // not covering the monitor
    CHECK_FALSE(GameDrawsCursor(false, true, false, false));  // locked game or Inspect
    CHECK_FALSE(GameDrawsCursor(true, true, false, true));    // Wind hid it (hotkey, setting)
}

TEST_CASE("the lag is one display frame by default, 0 turns it off, explicit values are capped") {
    CHECK(GameCursorLagMs(-1, 144.0) == doctest::Approx(1000.0 / 144.0));
    CHECK(GameCursorLagMs(-1, 0.0) == doctest::Approx(1000.0 / 60.0));
    CHECK(GameCursorLagMs(0, 144.0) == 0.0);
    CHECK(GameCursorLagMs(12, 144.0) == doctest::Approx(12.0));
    CHECK(GameCursorLagMs(500, 144.0) == doctest::Approx(100.0));
}

TEST_CASE("the pointer history reads back an earlier position, interpolated") {
    PointerHistory h;
    double x = -1, y = -1;
    CHECK_FALSE(h.at(0.0, x, y));
    h.push(0.0, 100, 10);
    h.push(10.0, 200, 20);
    h.push(20.0, 300, 30);
    CHECK(h.at(25.0, x, y)); CHECK(x == doctest::Approx(300)); CHECK(y == doctest::Approx(30));   // newest
    CHECK(h.at(20.0, x, y)); CHECK(x == doctest::Approx(300));
    CHECK(h.at(15.0, x, y)); CHECK(x == doctest::Approx(250)); CHECK(y == doctest::Approx(25));
    CHECK(h.at(3.0, x, y));  CHECK(x == doctest::Approx(130));
    CHECK(h.at(-5.0, x, y)); CHECK(x == doctest::Approx(100));   // older than the oldest: the oldest
    h.clear();
    CHECK_FALSE(h.at(0.0, x, y));
}

TEST_CASE("the pointer history wraps and keeps the newest samples") {
    PointerHistory h;
    for (int i = 0; i < PointerHistory::kN + 10; ++i) h.push(i * 7.0, i, 0);
    double x = 0, y = 0;
    const double newest = (PointerHistory::kN + 9) * 7.0;
    CHECK(h.at(newest - 7.0 * 1.5, x, y)); CHECK(x == doctest::Approx(PointerHistory::kN + 9 - 1.5));
    CHECK(h.at(0.0, x, y)); CHECK(x == doctest::Approx(10));   // the oldest kept sample
}
