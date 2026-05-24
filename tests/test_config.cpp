#include "doctest.h"
#include "../src/config.h"
using namespace wind;

TEST_CASE("defaults when text is empty") {
    Config c = ParseConfig("");
    CHECK(c.zoomInButton  == 2);   // XBUTTON2
    CHECK(c.zoomOutButton == 1);   // XBUTTON1
    CHECK(c.maxLevel == doctest::Approx(8.0));
    CHECK(c.fullRangeSeconds == doctest::Approx(1.2));
    CHECK(c.sensitivity == doctest::Approx(1.0));
}
TEST_CASE("parses overrides and ignores comments/blank lines") {
    const char* ini =
        "; comment\n"
        "maxLevel = 12.5\n"
        "\n"
        "zoomInButton=1\n"
        "sensitivity = 0.5\n";
    Config c = ParseConfig(ini);
    CHECK(c.maxLevel == doctest::Approx(12.5));
    CHECK(c.zoomInButton == 1);
    CHECK(c.sensitivity == doctest::Approx(0.5));
    CHECK(c.fullRangeSeconds == doctest::Approx(1.2)); // untouched default
}
TEST_CASE("malformed lines are ignored, keep defaults") {
    Config c = ParseConfig("garbage line\nmaxLevel\n=5\n");
    CHECK(c.maxLevel == doctest::Approx(8.0));
}
