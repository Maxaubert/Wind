#include "doctest.h"
#include "../src/config.h"

using namespace wind;

TEST_CASE("model defaults to render") {
    Config c = ParseConfig("");
    CHECK(c.model == "render");
    CHECK(c.fastPan == 1);
    CHECK(c.smoothPan == 0);
    CHECK(c.cursorSprite == 1);
}

TEST_CASE("model=transform parses and transform keys read") {
    Config c = ParseConfig("model=transform\nfastPan=0\nsmoothPan=1\ncursorSprite=0\n");
    CHECK(c.model == "transform");
    CHECK(c.fastPan == 0);
    CHECK(c.smoothPan == 1);
    CHECK(c.cursorSprite == 0);
}

TEST_CASE("unknown model value falls back to render") {
    Config c = ParseConfig("model=bogus\n");
    CHECK(c.model == "render");
}

TEST_CASE("FlipModel alternates render and transform") {
    CHECK(FlipModel("render") == "transform");
    CHECK(FlipModel("transform") == "render");
    // round-trips
    CHECK(FlipModel(FlipModel("render")) == "render");
    CHECK(FlipModel(FlipModel("transform")) == "transform");
}

TEST_CASE("FlipModel maps an unknown value to transform") {
    CHECK(FlipModel("bogus") == "transform");
    CHECK(FlipModel("") == "transform");
}
