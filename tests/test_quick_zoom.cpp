#include "doctest.h"
#include "../src/zoom_controller.h"
using namespace wind;

TEST_CASE("double-tap inside the window fires once") {
    QuickZoomDetector d;
    d.setWindow(0.3);
    CHECK(d.update(true, false, 0.00) == false);   // first in-tap
    CHECK(d.update(true, false, 0.20) == true);    // second in-tap within 0.3s
    CHECK(d.update(false, false, 0.21) == false);  // no edge -> nothing
}
TEST_CASE("two taps outside the window do not fire") {
    QuickZoomDetector d;
    d.setWindow(0.3);
    CHECK(d.update(true, false, 0.00) == false);
    CHECK(d.update(true, false, 0.50) == false);   // 0.5s gap > window; just rearms
    CHECK(d.update(true, false, 0.60) == true);    // now within window of the 0.50 tap
}
TEST_CASE("channels are independent - in then out does not fire") {
    QuickZoomDetector d;
    d.setWindow(0.3);
    CHECK(d.update(true, false, 0.00) == false);   // in tap
    CHECK(d.update(false, true, 0.10) == false);   // out tap (different channel)
}
TEST_CASE("either channel double-tapped fires") {
    QuickZoomDetector d;
    d.setWindow(0.3);
    CHECK(d.update(false, true, 0.00) == false);
    CHECK(d.update(false, true, 0.15) == true);    // out double-tap
}
TEST_CASE("triple-tap = one fire then a fresh sequence") {
    QuickZoomDetector d;
    d.setWindow(0.3);
    CHECK(d.update(true, false, 0.00) == false);
    CHECK(d.update(true, false, 0.10) == true);    // tap 2 fires + consumes
    CHECK(d.update(true, false, 0.20) == false);   // tap 3 starts a new sequence
    CHECK(d.update(true, false, 0.30) == true);    // tap 4 fires
}
TEST_CASE("a changed window is respected") {
    QuickZoomDetector d;
    d.setWindow(0.1);
    CHECK(d.update(true, false, 0.00) == false);
    CHECK(d.update(true, false, 0.20) == false);   // 0.2s > 0.1s window -> rearm, no fire
}
TEST_CASE("reset clears pending taps") {
    QuickZoomDetector d;
    d.setWindow(0.3);
    CHECK(d.update(true, false, 0.00) == false);
    d.reset();
    CHECK(d.update(true, false, 0.10) == false);   // first tap was cleared
}
TEST_CASE("a tap exactly at the window edge still fires (<= boundary)") {
    QuickZoomDetector d;
    d.setWindow(0.3);
    CHECK(d.update(true, false, 0.00) == false);
    CHECK(d.update(true, false, 0.30) == true);    // delta == window_ -> inclusive, fires
}
TEST_CASE("both channels edging in one call: a double-tap on either still fires") {
    QuickZoomDetector d;
    d.setWindow(0.3);
    CHECK(d.update(true, true, 0.00) == false);     // first tap on both channels
    CHECK(d.update(true, true, 0.10) == true);      // second tap within window on both
}

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
