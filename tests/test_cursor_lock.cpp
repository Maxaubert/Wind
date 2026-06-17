#include "doctest.h"
#include "../src/cursor_lock.h"

using wind::CursorLockController;

TEST_CASE("starts free") {
    CursorLockController c;
    CHECK(!c.locked());
}

TEST_CASE("toggle locks; toggle again unlocks") {
    CursorLockController c;
    c.toggle();
    CHECK(c.locked());
    c.toggle();
    CHECK(!c.locked());
}

TEST_CASE("toggle flips regardless of zoom (Inspect mode works at 1x)") {
    // The controller is zoom-agnostic; the tick decides what locked() means at each zoom level.
    CursorLockController c;
    c.toggle();
    CHECK(c.locked());
    c.toggle();
    CHECK(!c.locked());
    c.toggle();
    CHECK(c.locked());
}

TEST_CASE("reset returns to free from any state") {
    CursorLockController c;
    c.toggle();
    REQUIRE(c.locked());
    c.reset();
    CHECK(!c.locked());
}
