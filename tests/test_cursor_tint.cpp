#include "doctest.h"
#include "../src/cursor_tint_pixels.h"
#include <vector>
using namespace wind;

TEST_CASE("tint scales RGB by the matrix and keeps alpha") {
    const ColorMatrix m = BuildColorMatrix(1.0, 1.0, false);   // warmth 100%: 1200 K, encoded
    const uint32_t white = 0x80FFFFFFu;                        // half-transparent white
    const uint32_t t = TintPixel(white, m);
    CHECK((t >> 24) == 0x80u);                                 // alpha untouched
    CHECK(((t >> 16) & 0xFF) == 255u);                         // red kept
    CHECK(((t >> 8) & 0xFF) == (uint32_t)(SrgbEncode(0.0774) * 255 + 0.5));   // green per the table
    CHECK((t & 0xFF) == 0u);                                   // no blue at 1200 K
}
TEST_CASE("the identity leaves a pointer untouched, and dimming scales it") {
    std::vector<uint32_t> px = { 0xFF102030u, 0x00FFFFFFu, 0xFFFFFFFFu };
    std::vector<uint32_t> orig = px;
    TintArgb(px.data(), (int)px.size(), IdentityColorMatrix());
    CHECK(px == orig);
    TintArgb(px.data(), (int)px.size(), BuildColorMatrix(0.0, 0.5, false));
    CHECK(px[2] == 0xFF808080u);   // white at 50% brightness (128 = round(127.5))
}
TEST_CASE("alpha comes from the AND mask when the colour bitmap has none") {
    uint32_t px[2] = { 0x00FF0000u, 0x0000FF00u };
    CHECK_FALSE(AnyAlpha(px, 2));
    const uint8_t andBits[4] = { 0x40, 0, 0, 0 };   // x=0 opaque, x=1 transparent
    AlphaFromMask(px, 2, 1, andBits, 4);
    CHECK(px[0] == 0xFFFF0000u);
    CHECK(px[1] == 0x0000FF00u);
    CHECK(AnyAlpha(px, 2));
}
TEST_CASE("a monochrome pointer becomes colour: black, white, transparent, inverting-with-outline") {
    // 4x1 row: [AND0 XOR0][AND0 XOR1][AND1 XOR0][AND1 XOR1]
    const uint8_t andBits[4] = { 0x30, 0, 0, 0 };   // bits: 0 0 1 1
    const uint8_t xorBits[4] = { 0x50, 0, 0, 0 };   // bits: 0 1 0 1
    uint32_t out[4];
    MonoToArgb(andBits, xorBits, 4, 4, 1, out);
    CHECK(out[0] == 0xFF000000u);   // black
    CHECK(out[1] == 0xFFFFFFFFu);   // white
    CHECK(out[2] == 0xFF000000u);   // transparent, but next to the inverting pixel: outline
    CHECK(out[3] == 0xFFFFFFFFu);   // inverting pixel drawn white
}
TEST_CASE("only a pointer of invert and transparent pixels uses the invert blend") {
    const uint8_t andAll[4] = { 0xF0, 0, 0, 0 };    // 4x1, every AND bit 1
    const uint8_t xorOne[4] = { 0x40, 0, 0, 0 };    // one XOR bit set
    const uint8_t xorNone[4] = { 0, 0, 0, 0 };
    CHECK(MonoIsPureInvert(andAll, xorOne, 4, 4, 1));
    CHECK_FALSE(MonoIsPureInvert(andAll, xorNone, 4, 4, 1));   // nothing to invert at all
    const uint8_t andOpaque[4] = { 0x70, 0, 0, 0 };            // x=3 is opaque
    CHECK_FALSE(MonoIsPureInvert(andOpaque, xorOne, 4, 4, 1));
}
TEST_CASE("the outline only hugs inverting pixels; plain transparency stays clear") {
    // 5x1: [invert][transparent][transparent][transparent][transparent]
    const uint8_t andBits[4] = { 0xF8, 0, 0, 0 };   // all 1
    const uint8_t xorBits[4] = { 0x80, 0, 0, 0 };   // only x=0
    uint32_t out[5];
    MonoToArgb(andBits, xorBits, 4, 5, 1, out);
    CHECK(out[0] == 0xFFFFFFFFu);
    CHECK(out[1] == 0xFF000000u);   // neighbour: outline
    CHECK(out[2] == 0x00000000u);   // two away: clear
    CHECK(out[4] == 0x00000000u);
}
