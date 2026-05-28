#include "doctest.h"
#include "../src/config.h"
using namespace wind;

TEST_CASE("defaults when text is empty") {
    Config c = ParseConfig("");
    CHECK(c.zoomInButton  == 2);   // XBUTTON2
    CHECK(c.zoomOutButton == 1);   // XBUTTON1
    CHECK(c.maxLevel == doctest::Approx(8.0));
    CHECK(c.diagnostics == 0);
}
TEST_CASE("parses renderer knobs") {
    Config c = ParseConfig(
        "cursorSensitivity=1.5\ncursorScaleWithZoom=0\nbilinear=1\n");
    CHECK(c.cursorSensitivity == doctest::Approx(1.5));
    CHECK(c.cursorScaleWithZoom == 0);
    CHECK(c.bilinear == 1);
}
TEST_CASE("renderer knobs have sane defaults") {
    Config c = ParseConfig("");
    CHECK(c.cursorSensitivity == doctest::Approx(1.0));
    CHECK(c.cursorScaleWithZoom == 1);
    CHECK(c.bilinear == 1);
    CHECK(c.sharpness == doctest::Approx(0.0));   // off by default
    CHECK(c.cursorSmoothing == doctest::Approx(0.8));
    CHECK(c.zorderBand == 0);                  // normal topmost by default
    CHECK(c.brightness == doctest::Approx(1.0));
    CHECK(c.hdrTonemap == 1);                  // on by default (no-op on SDR)
    CHECK(c.cursorVisibility == "auto");       // follow the focused app by default
    CHECK(c.vsync == 1);                       // vsync on by default
    CHECK(c.dwmFlush == 0);                     // plain vsync pacing by default (fewer stutters)
    CHECK(c.multiMonitor == 1);                // follow the cursor's monitor by default
    CHECK(c.cropCapture == 1);                 // crop the copy on full repaints by default
    CHECK(c.smoothZoom == 0);                  // linear (current) by default
    CHECK(c.zoomInSpeed == doctest::Approx(1.0));
    CHECK(c.zoomOutSpeed == doctest::Approx(1.0));
    CHECK(c.smoothZoomAccel == doctest::Approx(3.0));
    CHECK(c.smoothZoomRamp == doctest::Approx(0.6));
}
TEST_CASE("vsync and dwmFlush can be set") {
    CHECK(ParseConfig("vsync=0\n").vsync == 0);
    CHECK(ParseConfig("dwmFlush=0\n").dwmFlush == 0);
    CHECK(ParseConfig("dwmFlush=1\n").dwmFlush == 1);
}
TEST_CASE("keyboard zoom defaults PageUp/PageDown; recenter unbound; all parseable") {
    Config d = ParseConfig("");
    CHECK(d.zoomInVk == 33);    // VK_PRIOR (PageUp)
    CHECK(d.zoomOutVk == 34);   // VK_NEXT  (PageDown)
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
TEST_CASE("parses cursorSmoothing, z-order band, brightness") {
    Config c = ParseConfig("cursorSmoothing=0.7\nzorderBand=16\nbrightness=0.85\n");
    CHECK(c.cursorSmoothing == doctest::Approx(0.7));
    CHECK(c.zorderBand == 16);
    CHECK(c.brightness == doctest::Approx(0.85));
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
        "cursorSensitivity = 0.5\n";
    Config c = ParseConfig(ini);
    CHECK(c.maxLevel == doctest::Approx(12.5));
    CHECK(c.zoomInButton == 1);
    CHECK(c.cursorSensitivity == doctest::Approx(0.5));
    CHECK(c.zoomInSpeed == doctest::Approx(1.0)); // untouched default
}
TEST_CASE("malformed lines are ignored, keep defaults") {
    Config c = ParseConfig("garbage line\nmaxLevel\n=5\n");
    CHECK(c.maxLevel == doctest::Approx(8.0));
}
TEST_CASE("multiMonitor can be set") {
    CHECK(ParseConfig("multiMonitor=0\n").multiMonitor == 0);
    CHECK(ParseConfig("multiMonitor=1\n").multiMonitor == 1);
}
TEST_CASE("cropCapture can be set") {
    CHECK(ParseConfig("cropCapture=0\n").cropCapture == 0);
    CHECK(ParseConfig("cropCapture=1\n").cropCapture == 1);
}
TEST_CASE("onboarded defaults to 0 and parses") {
    CHECK(ParseConfig("").onboarded == 0);
    CHECK(ParseConfig("onboarded=1\n").onboarded == 1);
}
TEST_CASE("alternate zoom VK keys default 0 and parse") {
    Config d = ParseConfig("");
    CHECK(d.zoomInVk2 == 0);
    CHECK(d.zoomOutVk2 == 0);
    Config c = ParseConfig("zoomInVk2=112\nzoomOutVk2=113\n");
    CHECK(c.zoomInVk2 == 112);
    CHECK(c.zoomOutVk2 == 113);
}
TEST_CASE("hide cursor hotkey defaults 0 and parses") {
    Config d = ParseConfig("");
    CHECK(d.hideCursorVk == 0);
    CHECK(d.hideCursorMods == 0);
    Config c = ParseConfig("hideCursorVk=72\nhideCursorMods=1\n");
    CHECK(c.hideCursorVk == 72);  // 'H'
    CHECK(c.hideCursorMods == 1); // Ctrl
}
TEST_CASE("modifier masks default 0 and parse") {
    Config d = ParseConfig("");
    CHECK(d.zoomInMods == 0);  CHECK(d.zoomOutMods == 0);
    CHECK(d.zoomInMods2 == 0); CHECK(d.zoomOutMods2 == 0);
    Config c = ParseConfig("zoomInMods=3\nzoomOutMods=1\nzoomInMods2=8\nzoomOutMods2=4\n");
    CHECK(c.zoomInMods == 3);  CHECK(c.zoomOutMods == 1);   // Ctrl+Alt / Ctrl
    CHECK(c.zoomInMods2 == 8); CHECK(c.zoomOutMods2 == 4);  // Win / Shift
}
TEST_CASE("zoom-speed and smooth-zoom knobs parse") {
    Config c = ParseConfig(
        "smoothZoom=1\nzoomInSpeed=2.0\nzoomOutSpeed=0.5\n"
        "smoothZoomAccel=4.0\nsmoothZoomRamp=0.25\n");
    CHECK(c.smoothZoom == 1);
    CHECK(c.zoomInSpeed == doctest::Approx(2.0));
    CHECK(c.zoomOutSpeed == doctest::Approx(0.5));
    CHECK(c.smoothZoomAccel == doctest::Approx(4.0));
    CHECK(c.smoothZoomRamp == doctest::Approx(0.25));
}
