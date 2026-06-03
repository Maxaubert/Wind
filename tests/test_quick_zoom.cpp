#include "doctest.h"
#include "../src/zoom_controller.h"
using namespace wind;

TEST_CASE("ApplyQuickZoom: zoomed above 200% snaps out and remembers") {
    QuickZoomResult r = ApplyQuickZoom(/*cur*/5.0, /*stored*/0.0, /*def*/4.0, /*max*/12.0);
    CHECK(r.newLevel  == doctest::Approx(1.0));
    CHECK(r.newStored == doctest::Approx(5.0));
}
TEST_CASE("ApplyQuickZoom: shallow zoom (<=200%) snaps out but is NOT remembered") {
    QuickZoomResult r = ApplyQuickZoom(/*cur*/1.5, /*stored*/5.0, /*def*/4.0, /*max*/12.0);
    CHECK(r.newLevel  == doctest::Approx(1.0));
    CHECK(r.newStored == doctest::Approx(5.0));   // prior memory preserved
}
TEST_CASE("ApplyQuickZoom: at 0% with a stored level snaps in to it (clamped to max)") {
    QuickZoomResult in = ApplyQuickZoom(/*cur*/1.0, /*stored*/5.0, /*def*/4.0, /*max*/12.0);
    CHECK(in.newLevel == doctest::Approx(5.0));
    QuickZoomResult clamped = ApplyQuickZoom(/*cur*/1.0, /*stored*/20.0, /*def*/4.0, /*max*/12.0);
    CHECK(clamped.newLevel == doctest::Approx(12.0));
}
TEST_CASE("ApplyQuickZoom: at 0% with nothing stored uses the default") {
    QuickZoomResult r = ApplyQuickZoom(/*cur*/1.0, /*stored*/0.0, /*def*/4.0, /*max*/12.0);
    CHECK(r.newLevel  == doctest::Approx(4.0));
    CHECK(r.newStored == doctest::Approx(0.0));    // memory unchanged on snap-in
}
TEST_CASE("ApplyQuickZoom: a default above maxLevel is clamped to maxLevel") {
    QuickZoomResult r = ApplyQuickZoom(/*cur*/1.0, /*stored*/0.0, /*def*/20.0, /*max*/12.0);
    CHECK(r.newLevel == doctest::Approx(12.0));
}
TEST_CASE("ApplyQuickZoom: exactly 200% is NOT remembered (boundary is strictly above)") {
    QuickZoomResult r = ApplyQuickZoom(/*cur*/2.0, /*stored*/0.0, /*def*/4.0, /*max*/12.0);
    CHECK(r.newLevel  == doctest::Approx(1.0));   // still snaps out
    CHECK(r.newStored == doctest::Approx(0.0));   // 2.0 is not > 2.0, so not remembered
}
