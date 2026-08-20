#include "doctest.h"
#include "../src/launch_quiesce.h"

using namespace wind;

// Measured on the rig, 2026-08-18: the Snipping Tool capture overlay while a zoom was live.
//   exe=ScreenClippingHost age=1s rect=0,0 3840x2160 style=0x94000000 ex=0x200008
static const unsigned long kSnipOverlayEx = 0x00200008ul;  // NOREDIRECTIONBITMAP | TOPMOST

TEST_CASE("launch quiesce: the snip overlay never arms the hold") {
    // The regression this exists for: a borderless fullscreen cover from a 1s-old process, which
    // passed the old shape-only test and froze the magnifier for 1.5s on every snip.
    CHECK(IsOverlayCover(kSnipOverlayEx));
    CHECK_FALSE(ShouldArmLaunchQuiesce(true, true, kSnipOverlayEx, true));
}

TEST_CASE("launch quiesce: a launching game still arms it") {
    CHECK_FALSE(IsOverlayCover(0ul));
    CHECK(ShouldArmLaunchQuiesce(true, true, 0ul, true));

    // Topmost alone must NOT disqualify - fullscreen games set it, and it says nothing about how
    // the window presents.
    const unsigned long kExTopmost = 0x00000008ul;
    CHECK_FALSE(IsOverlayCover(kExTopmost));
    CHECK(ShouldArmLaunchQuiesce(true, true, kExTopmost, true));
}

TEST_CASE("launch quiesce: every overlay style is vetoed") {
    static const unsigned long kOverlayBits[] = {
        kExTransparent, kExToolWindow, kExLayered, kExNoRedirectionBitmap, kExNoActivate,
    };
    for (unsigned long bit : kOverlayBits) {
        CHECK(IsOverlayCover(bit));
        CHECK_FALSE(ShouldArmLaunchQuiesce(true, true, bit, true));
        CHECK(IsOverlayCover(bit | 0x00000008ul));   // still an overlay alongside topmost
    }
}

TEST_CASE("launch quiesce: the original conditions are all still required") {
    CHECK_FALSE(ShouldArmLaunchQuiesce(false, true,  0ul, true));   // does not cover the monitor
    CHECK_FALSE(ShouldArmLaunchQuiesce(true,  false, 0ul, true));   // has a caption
    CHECK_FALSE(ShouldArmLaunchQuiesce(true,  true,  0ul, false));  // process older than the window
}
