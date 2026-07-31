#include "doctest.h"
#include "../src/hdr_scale.h"

using wind::AcceptSdrWhiteNits;
using wind::ScRgbScale;
using wind::ShouldRefreshSdrWhite;

// THE BUG (HDR brightness step on zoom-in/out). Round trip for one SDR pixel of sRGB value v while
// Windows HDR is on and the desktop is composited at `actual` nits of SDR white:
//   DWM composites it into the scRGB desktop we capture:  L = lin(v) * actual/80
//   our shader scales by ScRgbScale(assumed) = 80/assumed: L * 80/assumed = lin(v) * actual/assumed
//   we sRGB-encode that and present it on an SDR (BGRA8) overlay, so DWM re-applies the white level
//   when it composites US:                                 lin(v) * (actual/assumed) * (actual/80)
// Unzoomed baseline is lin(v) * actual/80, so the visible zoomed/baseline ratio is actual/assumed -
// which is ScRgbScale(assumed) / ScRgbScale(actual). It is 1.0 only when the scale tracks the LIVE
// slider value. Wind sampled the white level once per device build, so every later slider move left
// a permanent step: below the cached point the zoom darkened, above it brightened, and at exactly
// the cached point it matched. Measured on the reporting machine: cached 180 nits, live 128 nits.
static double VisibleRatio(double actualNits, double assumedNits) {
    return (double)ScRgbScale(assumedNits) / (double)ScRgbScale(actualNits);
}

TEST_CASE("a stale SDR white level is exactly the reported brightness step") {
    CHECK(VisibleRatio(128.0, 180.0) == doctest::Approx(128.0 / 180.0));   // darker (the report)
    CHECK(VisibleRatio(240.0, 180.0) == doctest::Approx(240.0 / 180.0));   // brighter (the report)
    CHECK(VisibleRatio(180.0, 180.0) == doctest::Approx(1.0));             // the one matching point
}

TEST_CASE("tracking the live white level makes the zoom transition invisible at every slider position") {
    // The fix: whatever the slider is at, the scale is built from that same value -> ratio 1.0.
    const double kSliderNits[] = { 80.0, 128.0, 180.0, 200.0, 240.0, 480.0 };
    for (double nits : kSliderNits)
        CHECK(VisibleRatio(nits, nits) == doctest::Approx(1.0));
}

TEST_CASE("ScRgbScale maps SDR white to 1.0 and refuses junk input") {
    CHECK(ScRgbScale(80.0) == doctest::Approx(1.0f));     // scRGB 1.0 IS 80 nits
    CHECK(ScRgbScale(160.0) == doctest::Approx(0.5f));
    // Unknown / not-yet-queried values must fall back to passthrough, never a wild scale.
    CHECK(ScRgbScale(0.0) == doctest::Approx(1.0f));
    CHECK(ScRgbScale(-5.0) == doctest::Approx(1.0f));
    CHECK(ScRgbScale(1.0) == doctest::Approx(1.0f));
}

TEST_CASE("a failed white-level query keeps the last known good value") {
    // Snapping to a default on a transient query failure would itself be a visible brightness step.
    CHECK(AcceptSdrWhiteNits(0.0, 128.0) == doctest::Approx(128.0));
    CHECK(AcceptSdrWhiteNits(-1.0, 128.0) == doctest::Approx(128.0));
    CHECK(AcceptSdrWhiteNits(240.0, 128.0) == doctest::Approx(240.0));   // a good read wins
}

TEST_CASE("the live re-read is throttled, and only on the FP16 HDR capture path") {
    // ~0.2 ms per DisplayConfig round trip: far too costly to pay per frame at 144 Hz.
    CHECK(ShouldRefreshSdrWhite(true, 1000, 0, 500));         // never sampled yet -> go
    CHECK(ShouldRefreshSdrWhite(true, 1500, 1000, 500));      // interval elapsed -> go
    CHECK_FALSE(ShouldRefreshSdrWhite(true, 1499, 1000, 500));// inside the interval -> skip
    CHECK_FALSE(ShouldRefreshSdrWhite(false, 9999, 0, 500));  // SDR/BGRA8 capture -> never query
}
