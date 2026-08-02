#include "doctest.h"
#include "../src/key_state.h"

using wind::EffectiveKeyDown;

// issue #167: the raw-input shadow may VETO a held reading, never assert one.

TEST_CASE("primary up is up, regardless of the shadow") {
    CHECK_FALSE(EffectiveKeyDown(false, true,  true));
    CHECK_FALSE(EffectiveKeyDown(false, true,  false));
    CHECK_FALSE(EffectiveKeyDown(false, false, true));
    CHECK_FALSE(EffectiveKeyDown(false, false, false));
}

TEST_CASE("healthy hold: primary down + shadow down = down") {
    CHECK(EffectiveKeyDown(true, true, true));
}

TEST_CASE("the stuck case: primary down but the physical key is up - vetoed") {
    CHECK_FALSE(EffectiveKeyDown(true, true, false));
}

TEST_CASE("no raw input ever observed: primary stands alone (bootstrap / registration failure)") {
    CHECK(EffectiveKeyDown(true, false, false));
    CHECK(EffectiveKeyDown(true, false, true));
}
