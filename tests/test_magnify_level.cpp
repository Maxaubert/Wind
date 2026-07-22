#include "doctest.h"
#include "../src/magnify_level.h"

using wind::MagnifyTargetPct;

TEST_CASE("MagnifyTargetPct: smooth level to integer percent") {
    CHECK(MagnifyTargetPct(1.0) == 100);
    CHECK(MagnifyTargetPct(1.37) == 137);      // arbitrary percents apply exactly (measured)
    CHECK(MagnifyTargetPct(2.0) == 200);
    CHECK(MagnifyTargetPct(2.999) == 300);     // nearest percent
    CHECK(MagnifyTargetPct(1.004) == 100);   // 1.005 is not exactly representable (100.4999...),
    CHECK(MagnifyTargetPct(1.006) == 101);   // so test the rounding boundary with clean values
}

TEST_CASE("MagnifyTargetPct: mandatory clamps") {
    // Below 1x clamps up (Magnifier has no sub-1x).
    CHECK(MagnifyTargetPct(0.5) == 100);
    // Above 1600% Magnifier IGNORES the write (measured), so the clamp must happen here.
    CHECK(MagnifyTargetPct(16.0) == 1600);
    CHECK(MagnifyTargetPct(20.0) == 1600);
    CHECK(MagnifyTargetPct(1e9) == 1600);
}
