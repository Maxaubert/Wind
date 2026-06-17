#include "doctest.h"
#include "../src/cursor_lock.h"

using wind::CursorLockController;

TEST_CASE("starts free") {
    CursorLockController c;
    CHECK(!c.locked());
    CHECK(!c.panFromRaw());
}

TEST_CASE("toggle while zoomed locks; toggle again unlocks") {
    CursorLockController c;
    c.toggle(/*zoomedIn=*/true);
    CHECK(c.locked());
    CHECK(c.panFromRaw());
    c.toggle(true);
    CHECK(!c.locked());
}

TEST_CASE("toggle while NOT zoomed is ignored") {
    CursorLockController c;
    c.toggle(/*zoomedIn=*/false);
    CHECK(!c.locked());
}

TEST_CASE("commitClick unlocks only when locked") {
    CursorLockController c;
    c.commitClick();              // not locked: no-op
    CHECK(!c.locked());
    c.toggle(true);
    REQUIRE(c.locked());
    c.commitClick();
    CHECK(!c.locked());
}

TEST_CASE("reset returns to free from any state") {
    CursorLockController c;
    c.toggle(true);
    REQUIRE(c.locked());
    c.reset();
    CHECK(!c.locked());
}

TEST_CASE("toggle while NOT zoomed does not clear an existing lock") {
    CursorLockController c;
    c.toggle(/*zoomedIn=*/true);
    REQUIRE(c.locked());
    c.toggle(/*zoomedIn=*/false);  // unzoom-toggle is a no-op; lock survives
    CHECK(c.locked());
    c.reset();                     // only reset() clears the lock
    CHECK(!c.locked());
}

TEST_CASE("commitClick is idempotent while unlocked") {
    CursorLockController c;
    c.commitClick();
    c.commitClick();
    CHECK(!c.locked());
}
