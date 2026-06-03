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
