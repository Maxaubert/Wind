#include "doctest.h"
#include "../src/detached_view.h"
using namespace wind;

TEST_CASE("detached map: view from the given centre, cursor fields from the real pointer") {
    MapResult r = DetachedMap(1920, 1080, 1000, 500, 4, 3840, 2160);
    CHECK(r.centerX == doctest::Approx(1920)); CHECK(r.centerY == doctest::Approx(1080));
    CHECK(r.srcLeft == doctest::Approx(1920 - 480)); CHECK(r.srcTop == doctest::Approx(1080 - 270));
    CHECK(r.clickDesktopX == 1000); CHECK(r.clickDesktopY == 500);
    CHECK(r.cursorScreenX == doctest::Approx((1000 - 1440) * 4.0));   // off to the left: not visible
    CHECK(r.cursorScreenY == doctest::Approx((500 - 810) * 4.0));
}
