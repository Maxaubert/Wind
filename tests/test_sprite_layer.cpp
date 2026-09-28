#include "doctest.h"
#include "../src/sprite_layer.h"

using wind::PickSpriteLayer;
using wind::SpriteLayer;

TEST_CASE("sprite layer: auto off or no band-16 window always means the low window") {
    CHECK(PickSpriteLayer(false, true, 0) == SpriteLayer::Low);
    CHECK(PickSpriteLayer(false, true, 17) == SpriteLayer::Low);
    CHECK(PickSpriteLayer(true, false, 0) == SpriteLayer::Low);
    CHECK(PickSpriteLayer(true, false, 1) == SpriteLayer::Low);
}

TEST_CASE("sprite layer: ordinary foregrounds get the band-16 window, above thumbnails and Start") {
    CHECK(PickSpriteLayer(true, true, 0) == SpriteLayer::High);    // unknown
    CHECK(PickSpriteLayer(true, true, 1) == SpriteLayer::High);    // ZBID_DESKTOP, normal apps
    CHECK(PickSpriteLayer(true, true, 2) == SpriteLayer::High);    // UIAccess apps
    CHECK(PickSpriteLayer(true, true, 16) == SpriteLayer::High);   // Start, flyouts, thumbnails
}

TEST_CASE("sprite layer: a foreground above band 16 (the snip overlay is 17) gets the low window") {
    CHECK(PickSpriteLayer(true, true, 17) == SpriteLayer::Low);
    CHECK(PickSpriteLayer(true, true, 18) == SpriteLayer::Low);
}
