#include "doctest.h"
#include "sprite_layer.h"

using wind::PickSpriteLayer;
using wind::SpriteLayer;

TEST_CASE("sprite shape fit: small shapes keep their size on the base canvas") {
    const auto f = wind::FitSpriteShape(32, 32, 0, 0);
    CHECK(f.canvas == 64);
    CHECK(f.drawW == 32); CHECK(f.drawH == 32);
    CHECK(f.hotX == 0);   CHECK(f.hotY == 0);
}

TEST_CASE("sprite shape fit: a large pointer draws at its native size with its native hotspot") {
    const auto f = wind::FitSpriteShape(96, 96, 48, 48);   // pointer size 5, centred I-beam hotspot
    CHECK(f.canvas == 96);
    CHECK(f.drawW == 96); CHECK(f.drawH == 96);
    CHECK(f.hotX == 48);  CHECK(f.hotY == 48);
    const auto g = wind::FitSpriteShape(256, 256, 10, 20);  // the largest shape that fits unscaled
    CHECK(g.canvas == 256);
    CHECK(g.drawW == 256); CHECK(g.hotX == 10); CHECK(g.hotY == 20);
}

TEST_CASE("sprite shape fit: over the cap, shape and hotspot scale together") {
    const auto f = wind::FitSpriteShape(512, 512, 256, 100);
    CHECK(f.canvas == 256);
    CHECK(f.drawW == 256); CHECK(f.drawH == 256);
    CHECK(f.hotX == 128);  CHECK(f.hotY == 50);
}

TEST_CASE("sprite shape fit: an unreadable size falls back to 32 px") {
    const auto f = wind::FitSpriteShape(0, -1, 5, 5);
    CHECK(f.canvas == 64);
    CHECK(f.drawW == 32); CHECK(f.drawH == 32);
    CHECK(f.hotX == 5);   CHECK(f.hotY == 5);
}

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
