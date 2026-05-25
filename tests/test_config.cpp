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
    CHECK(c.cursorSmoothing == doctest::Approx(0.5));
    CHECK(c.motionBlur == 0);                  // off by default
    CHECK(c.motionBlurStrength == doctest::Approx(1.0));
    CHECK(c.zorderBand == 0);                  // normal topmost by default
    CHECK(c.brightness == doctest::Approx(1.0));
    CHECK(c.hdrTonemap == 1);                  // on by default (no-op on SDR)
    CHECK(c.cursorVisibility == "auto");       // follow the focused app by default
    CHECK(c.vsync == 1);                       // vsync on by default
    CHECK(c.dwmFlush == 1);                     // DwmFlush pacing on by default (the smooth path)
    CHECK(c.tickHzCap == 0);                    // 0 = auto-detect display refresh rate
}
TEST_CASE("vsync, dwmFlush, tickHzCap can be set") {
    CHECK(ParseConfig("vsync=0\n").vsync == 0);
    CHECK(ParseConfig("dwmFlush=0\n").dwmFlush == 0);
    CHECK(ParseConfig("dwmFlush=1\n").dwmFlush == 1);
    CHECK(ParseConfig("tickHzCap=240\n").tickHzCap == 240);
}
TEST_CASE("keyboard zoom / recenter Virtual-Key bindings parse; default unbound") {
    Config d = ParseConfig("");
    CHECK(d.zoomInVk == 0);
    CHECK(d.zoomOutVk == 0);
    CHECK(d.recenterVk == 0);
    Config c = ParseConfig("zoomInVk=33\nzoomOutVk=34\nrecenterVk=112\n");
    CHECK(c.zoomInVk == 33);
    CHECK(c.zoomOutVk == 34);
    CHECK(c.recenterVk == 112);
}
TEST_CASE("parses cursorVisibility") {
    CHECK(ParseConfig("cursorVisibility=always\n").cursorVisibility == "always");
    CHECK(ParseConfig("cursorVisibility=never\n").cursorVisibility == "never");
    CHECK(ParseConfig("cursorVisibility=auto\n").cursorVisibility == "auto");
}
TEST_CASE("hdrTonemap can be disabled") {
    Config c = ParseConfig("hdrTonemap=0\n");
    CHECK(c.hdrTonemap == 0);
}
TEST_CASE("parses cursorSmoothing, motion blur, z-order band, brightness") {
    Config c = ParseConfig("cursorSmoothing=0.7\nmotionBlur=1\nmotionBlurStrength=0.5\nzorderBand=16\nbrightness=0.85\n");
    CHECK(c.cursorSmoothing == doctest::Approx(0.7));
    CHECK(c.motionBlur == 1);
    CHECK(c.motionBlurStrength == doctest::Approx(0.5));
    CHECK(c.zorderBand == 16);
    CHECK(c.brightness == doctest::Approx(0.85));
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
