#include "doctest.h"
#include "../src/config.h"

using namespace wind;

TEST_CASE("model defaults to render, magnifyStep to 5") {
    Config c = ParseConfig("");
    CHECK(c.model == "render");
    CHECK(c.magnifyStep == 5);
}

TEST_CASE("model=magnify parses and magnifyStep reads") {
    Config c = ParseConfig("model=magnify\nmagnifyStep=25\n");
    CHECK(c.model == "magnify");
    CHECK(c.magnifyStep == 25);
}

TEST_CASE("legacy model=transform maps to magnify") {
    // The removed transform model's role (DRM-safe magnification) is taken over by magnify;
    // an ini written by an older build must keep working without falling back to render.
    Config c = ParseConfig("model=transform\n");
    CHECK(c.model == "magnify");
}

TEST_CASE("unknown model value falls back to render") {
    Config c = ParseConfig("model=bogus\n");
    CHECK(c.model == "render");
}

TEST_CASE("magnifyStep snaps to the supported Windows Magnifier increments") {
    CHECK(ParseConfig("magnifyStep=1\n").magnifyStep == 5);
    CHECK(ParseConfig("magnifyStep=7\n").magnifyStep == 5);
    CHECK(ParseConfig("magnifyStep=10\n").magnifyStep == 10);
    CHECK(ParseConfig("magnifyStep=17\n").magnifyStep == 10);
    CHECK(ParseConfig("magnifyStep=25\n").magnifyStep == 25);
    CHECK(ParseConfig("magnifyStep=40\n").magnifyStep == 50);
    CHECK(ParseConfig("magnifyStep=100\n").magnifyStep == 50);
    CHECK(ParseConfig("magnifyStep=-3\n").magnifyStep == 5);
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
