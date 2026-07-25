#include "doctest.h"
#include "../src/config.h"

using namespace wind;

TEST_CASE("model defaults to render") {
    Config c = ParseConfig("");
    CHECK(c.model == "render");
}

TEST_CASE("model=magnify parses") {
    Config c = ParseConfig("model=magnify\n");
    CHECK(c.model == "magnify");
}

TEST_CASE("magnifyStep parses and clamps to Windows' 5..400 range") {
    CHECK(ParseConfig("").magnifyStep == 50);              // shipped default
    CHECK(ParseConfig("magnifyStep=25\n").magnifyStep == 25);
    CHECK(ParseConfig("magnifyStep=1\n").magnifyStep == 5);
    CHECK(ParseConfig("magnifyStep=999\n").magnifyStep == 400);
    CHECK(ParseConfig("magnifyStep=-10\n").magnifyStep == 5);
}

TEST_CASE("model=transform is a first-class model again (issue #148 revival)") {
    Config c = ParseConfig("model=transform\n");
    CHECK(c.model == "transform");
}

TEST_CASE("transform-model knobs default and parse") {
    Config d = ParseConfig("");
    CHECK(d.fastPan == 1);
    CHECK(d.smoothPan == 0);
    CHECK(d.cursorSprite == 1);
    Config c = ParseConfig("fastPan=0\nsmoothPan=1\ncursorSprite=0\n");
    CHECK(c.fastPan == 0);
    CHECK(c.smoothPan == 1);
    CHECK(c.cursorSprite == 0);
}

TEST_CASE("unknown model value falls back to render") {
    Config c = ParseConfig("model=bogus\n");
    CHECK(c.model == "render");
}

TEST_CASE("FlipModel alternates render and magnify") {
    CHECK(FlipModel("render") == "magnify");
    CHECK(FlipModel("magnify") == "render");
    // round-trips
    CHECK(FlipModel(FlipModel("render")) == "render");
    CHECK(FlipModel(FlipModel("magnify")) == "magnify");
}

TEST_CASE("FlipModel maps an unknown value to magnify") {
    CHECK(FlipModel("bogus") == "magnify");
    CHECK(FlipModel("") == "magnify");
}
