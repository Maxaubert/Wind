#include "doctest.h"
#include "../src/crosshair.h"
using namespace wind;

static uint32_t at(const std::vector<uint32_t>& px, int n, int x, int y) { return px[(size_t)y * n + x]; }
static unsigned alpha(uint32_t p) { return (p >> 24) & 0xFFu; }

TEST_CASE("crosshair: size, center ink, transparent corners") {
    const int n = 48;
    auto px = BuildCrosshairBGRA(n, false);
    REQUIRE(px.size() == (size_t)n * n);
    // The design centers at (n-1)/2 = 23.5, which is the CENTER of texel 23 (texel 23 spans
    // [23,24)). So exactly texel (23,23) is fully covered core -> opaque light grey (0xCC);
    // its neighbours are core/outline mixes. The cross runs continuously through it (no gap).
    CHECK(at(px, n, 23, 23) == 0xFFCCCCCCu);
    // The arms are opaque core along their length too (x=23 column / y=23 row near the center).
    CHECK(at(px, n, 23, 10) == 0xFFCCCCCCu);
    CHECK(at(px, n, 10, 23) == 0xFFCCCCCCu);
    // Corners are far from both arms -> fully transparent.
    CHECK(at(px, n, 0, 0) == 0u);
    CHECK(at(px, n, n - 1, 0) == 0u);
    CHECK(at(px, n, 0, n - 1) == 0u);
    CHECK(at(px, n, n - 1, n - 1) == 0u);
}

TEST_CASE("crosshair: symmetric about texel 23's center; last row/col are margin") {
    const int n = 48;
    auto px = BuildCrosshairBGRA(n, false);
    // Reflection about 23.5 maps texel x -> texel (n-2)-x (texel 23 self-mirrors), so the
    // rightmost column / bottom row have no mirror partner - they are transparent margin.
    for (int i = 0; i < n; ++i) {
        CHECK(at(px, n, n - 1, i) == 0u);
        CHECK(at(px, n, i, n - 1) == 0u);
    }
    for (int y = 0; y < n - 1; ++y) for (int x = 0; x < n - 1; ++x) {
        CAPTURE(x); CAPTURE(y);
        CHECK(at(px, n, x, y) == at(px, n, n - 2 - x, y));          // mirror X
        CHECK(at(px, n, x, y) == at(px, n, x, n - 2 - y));          // mirror Y
        CHECK(at(px, n, x, y) == at(px, n, y, x));                  // diagonal (vArm<->hArm swap)
    }
}

TEST_CASE("crosshair: larger n only adds transparent margin (fixed arm length)") {
    auto a48 = BuildCrosshairBGRA(48, false);
    auto a64 = BuildCrosshairBGRA(64, false);
    // The 48 design sits centered in the 64 bitmap at offset (64-48)/2 = 8.
    for (int y = 0; y < 48; ++y) for (int x = 0; x < 48; ++x) {
        CHECK(a48[(size_t)y * 48 + x] == a64[(size_t)(y + 8) * 64 + (x + 8)]);
    }
    // Everything outside that centered 48x48 block is transparent margin.
    for (int y = 0; y < 64; ++y) for (int x = 0; x < 64; ++x) {
        if (x >= 8 && x < 56 && y >= 8 && y < 56) continue;
        CHECK(a64[(size_t)y * 64 + x] == 0u);
    }
}

TEST_CASE("crosshair: premultiplied channels never exceed alpha") {
    const int n = 48;
    auto straight = BuildCrosshairBGRA(n, false);
    auto pre = BuildCrosshairBGRA(n, true);
    for (size_t i = 0; i < pre.size(); ++i) {
        unsigned a = alpha(pre[i]);
        unsigned b = pre[i] & 0xFFu, g = (pre[i] >> 8) & 0xFFu, r = (pre[i] >> 16) & 0xFFu;
        CHECK(b <= a); CHECK(g <= a); CHECK(r <= a);
        // Same alpha as the straight-alpha build; channels are the straight ones scaled by a/255.
        CHECK(a == alpha(straight[i]));
        unsigned sb = straight[i] & 0xFFu;
        CHECK(b == sb * a / 255u);
    }
}
