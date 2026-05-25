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
    CHECK(c.diagnostics == 0);
    CHECK(c.updateMode == 0);
    CHECK(c.maxUpdateHz == 0);
}
TEST_CASE("parses engine selection and renderer knobs") {
    Config c = ParseConfig(
        "engine=render\ncursorSensitivity=1.5\ncursorScaleWithZoom=0\nbilinear=1\n");
    CHECK(c.engine == "render");
    CHECK(c.cursorSensitivity == doctest::Approx(1.5));
    CHECK(c.cursorScaleWithZoom == 0);
    CHECK(c.bilinear == 1);
}
TEST_CASE("engine defaults to render; renderer knobs have sane defaults") {
    Config c = ParseConfig("");
    CHECK(c.engine == "render");
    CHECK(c.cursorSensitivity == doctest::Approx(1.0));
    CHECK(c.cursorScaleWithZoom == 1);
    CHECK(c.bilinear == 1);
    CHECK(c.motionBlur == 1);
    CHECK(c.motionBlurStrength == doctest::Approx(1.0));
}
TEST_CASE("parses motion blur knobs") {
    Config c = ParseConfig("motionBlur=0\nmotionBlurStrength=0.5\n");
    CHECK(c.motionBlur == 0);
    CHECK(c.motionBlurStrength == doctest::Approx(0.5));
}
TEST_CASE("engine can select the Magnification API path") {
    Config c = ParseConfig("engine=mag\n");
    CHECK(c.engine == "mag");
}
TEST_CASE("parses diagnostics flag") {
    Config c = ParseConfig("diagnostics=1\n");
    CHECK(c.diagnostics == 1);
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
