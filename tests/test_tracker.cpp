#include "doctest.h"
#include "../src/tracker.h"
using namespace wind;

// --- Free mode (normal desktop) ------------------------------------------------

TEST_CASE("free mode follows the OS cursor when it moves") {
    Tracker t(1920, 1080, 1.0);
    t.update(100, 100, 0, 0);
    CHECK(t.centerX() == doctest::Approx(100));
    CHECK(t.centerY() == doctest::Approx(100));
    t.update(300, 200, 0, 0);
    CHECK(t.centerX() == doctest::Approx(300));
    CHECK(t.centerY() == doctest::Approx(200));
    CHECK_FALSE(t.locked());
}

TEST_CASE("holds still when neither cursor nor raw move") {
    Tracker t(1920, 1080, 1.0);
    t.update(700, 700, 0, 0);
    t.update(700, 700, 0, 0);
    CHECK(t.centerX() == doctest::Approx(700));
}

// --- Flicker regression (the reported bug) -------------------------------------

TEST_CASE("a lone frozen-cursor tick with raw movement does NOT jump the lens") {
    // During free movement an occasional tick samples an unchanged GetCursorPos while
    // raw deltas arrive (sampling alias). Integrating it would jump the lens off-centre
    // then snap back next tick = the flicker. The lens must hold instead.
    Tracker t(1920, 1080, 1.0);
    t.update(960, 540, 0, 0);
    t.update(960, 540, 50, 0);            // lone alias tick
    CHECK(t.centerX() == doctest::Approx(960));   // held, NOT 1010
    CHECK_FALSE(t.locked());
}

TEST_CASE("interleaved moved/frozen ticks track the cursor without oscillating") {
    Tracker t(1920, 1080, 1.0);
    t.update(500, 500, 0, 0);
    t.update(504, 500, 40, 0);            // moved -> snap, resets lock counter
    CHECK(t.centerX() == doctest::Approx(504));
    t.update(504, 500, 40, 0);            // alias tick -> hold (not 544)
    CHECK(t.centerX() == doctest::Approx(504));
    t.update(508, 500, 40, 0);            // moved -> snap to true cursor
    CHECK(t.centerX() == doctest::Approx(508));
    CHECK_FALSE(t.locked());              // never falsely locked
}

// --- Locked mode (games that hide/clip/lock the cursor) ------------------------

TEST_CASE("lock engages only after sustained freeze, then integrates raw deltas") {
    Tracker t(1920, 1080, 1.0);
    t.update(960, 540, 0, 0);                       // establish
    for (int i = 0; i < Tracker::kLockEngageTicks - 1; ++i)
        t.update(960, 540, 5, 0);                   // ramp: held, not integrated
    CHECK(t.centerX() == doctest::Approx(960));
    CHECK_FALSE(t.locked());
    t.update(960, 540, 5, 0);                       // threshold tick: locks + integrates
    CHECK(t.locked());
    CHECK(t.centerX() == doctest::Approx(965));
    t.update(960, 540, 10, 0);                      // stays locked, keeps integrating
    CHECK(t.centerX() == doctest::Approx(975));
}

TEST_CASE("sensitivity scales locked-mode panning") {
    Tracker t(1920, 1080, 2.0);
    t.update(960, 540, 0, 0);
    for (int i = 0; i < Tracker::kLockEngageTicks - 1; ++i)
        t.update(960, 540, 1, 0);                   // ramp (held)
    CHECK(t.centerX() == doctest::Approx(960));
    t.update(960, 540, 10, 0);                      // engages + integrates: 10 * 2.0
    CHECK(t.centerX() == doctest::Approx(980));
}

TEST_CASE("returns to free mode and snaps to cursor when it moves again") {
    Tracker t(1920, 1080, 1.0);
    t.update(960, 540, 0, 0);
    for (int i = 0; i < Tracker::kLockEngageTicks; ++i)
        t.update(960, 540, 50, 0);                  // engage + pan in locked mode
    CHECK(t.locked());
    t.update(400, 400, 0, 0);                       // OS cursor moved -> free, snap
    CHECK(t.centerX() == doctest::Approx(400));
    CHECK(t.centerY() == doctest::Approx(400));
    CHECK_FALSE(t.locked());
}

TEST_CASE("clamps center to screen bounds in locked mode") {
    Tracker t(1920, 1080, 1.0);
    t.update(10, 10, 0, 0);
    for (int i = 0; i < Tracker::kLockEngageTicks - 1; ++i)
        t.update(10, 10, -1, -1);                   // ramp (held)
    t.update(10, 10, -1000, -1000);                 // engages + integrates, clamps
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
