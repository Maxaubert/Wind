#include "doctest.h"
#include "../src/mpo_guard.h"

using namespace wind;

TEST_CASE("MPO guard: only zoomed, transform, MPO on and nearest") {
    CHECK(WantMpoGuard(true, true, true, 0) == true);
    CHECK(WantMpoGuard(false, true, true, 0) == false);   // 1x: keep Independent Flip
    CHECK(WantMpoGuard(true, false, true, 0) == false);   // render engine: no DWM zoom
    CHECK(WantMpoGuard(true, true, false, 0) == false);   // MPO off: no plane to overflow
    CHECK(WantMpoGuard(true, true, true, 1) == false);    // smooth already forces the layer
}

TEST_CASE("MPO guard matrix is not identity but visually a no-op") {
    const ColorMatrix g = MpoGuardMatrix();
    CHECK(IsIdentity(g) == false);
    for (int i = 0; i < 3; ++i) CHECK((1.0f - g.m[i][i]) * 255.0f < 1.0f);   // under one 8-bit step
    CHECK(g.m[3][3] == 1.0f);                                                // alpha untouched
    CHECK(g.m[4][0] == 0.0f);                                                // no offsets
}

TEST_CASE("a real colour filter is kept; identity becomes the guard only when wanted") {
    ColorMatrix warm = IdentityColorMatrix(); warm.m[2][2] = 0.7f;
    CHECK(SameMatrix(GuardedColorMatrix(warm, true), warm));
    CHECK(SameMatrix(GuardedColorMatrix(IdentityColorMatrix(), true), MpoGuardMatrix()));
    CHECK(IsIdentity(GuardedColorMatrix(IdentityColorMatrix(), false)));
}
