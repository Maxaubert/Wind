#include "doctest.h"
#include "../src/config.h"
using namespace wind;

TEST_CASE("defaults when text is empty") {
    Config c = ParseConfig("");
    CHECK(c.zoomInButton  == 0);   // shipped unbound (onboarding captures it)
    CHECK(c.zoomOutButton == 0);   // shipped unbound
    CHECK(c.maxLevel == doctest::Approx(12.0));
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
    CHECK(c.cursorSmoothing == doctest::Approx(0.4));
    CHECK(c.zorderBand == 16);                 // shipped 16 (UIAccess high band; falls back if unavailable)
    CHECK(c.brightness == doctest::Approx(1.0));
    CHECK(c.hdrTonemap == 1);                  // on by default (no-op on SDR)
    CHECK(c.cursorVisibility == "auto");       // follow the focused app by default
    CHECK(c.vsync == 1);                       // vsync on by default
    CHECK(c.dwmFlush == 0);                     // plain vsync pacing by default (fewer stutters)
    CHECK(c.multiMonitor == 0);                // shipped primary-only (follow-cursor opt-in)
    CHECK(c.cropCapture == 0);                 // shipped off: always copy all changed regions (no edge staleness)
    CHECK(c.smoothZoom == 1);                  // shipped on (eased-in zoom)
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
    CHECK(d.zoomInVk == 0);     // shipped unbound (onboarding captures it)
    CHECK(d.zoomOutVk == 0);
    CHECK(d.recenterVk == 0);
    Config c = ParseConfig("zoomInVk=33\nzoomOutVk=34\nrecenterVk=112\n");
    CHECK(c.zoomInVk == 33);
    CHECK(c.zoomOutVk == 34);
    CHECK(c.recenterVk == 112);
}
TEST_CASE("cursorLockVk: unbound by default, parseable, forbidden-sanitized") {
    Config d = ParseConfig("");
    CHECK(d.cursorLockVk == 0);
    CHECK(ParseConfig("cursorLockVk=113\n").cursorLockVk == 113);   // F2
    CHECK(ParseConfig("cursorLockVk=8\n").cursorLockVk == 0);       // Backspace -> sanitized to unbound
}
TEST_CASE("IsForbiddenBindVk blocks keys Wind must never swallow, allows the rest") {
    CHECK(IsForbiddenBindVk(0x01));   // VK_LBUTTON (left click)
    CHECK(IsForbiddenBindVk(0x02));   // VK_RBUTTON (right click)
    CHECK(IsForbiddenBindVk(0x08));   // VK_BACK (Backspace)
    CHECK(IsForbiddenBindVk(0x5B));   // VK_LWIN
    CHECK(IsForbiddenBindVk(0x5C));   // VK_RWIN
    // Common legitimate binds stay allowed.
    CHECK_FALSE(IsForbiddenBindVk(0));     // unbound
    CHECK_FALSE(IsForbiddenBindVk(33));    // PageUp
    CHECK_FALSE(IsForbiddenBindVk(34));    // PageDown
    CHECK_FALSE(IsForbiddenBindVk(112));   // F1
    CHECK_FALSE(IsForbiddenBindVk(107));   // NumPad +
}
TEST_CASE("ParseConfig sanitizes forbidden keybinds to unbound (defense in depth)") {
    // A hand-edited ini binding a forbidden key must come back as 0 (unbound), not the dangerous VK.
    CHECK(ParseConfig("zoomInVk=8\n").zoomInVk == 0);        // Backspace
    CHECK(ParseConfig("zoomOutVk=91\n").zoomOutVk == 0);     // LWin
    CHECK(ParseConfig("zoomInVk2=2\n").zoomInVk2 == 0);      // right click
    CHECK(ParseConfig("zoomOutVk2=1\n").zoomOutVk2 == 0);    // left click
    CHECK(ParseConfig("recenterVk=92\n").recenterVk == 0);   // RWin
    CHECK(ParseConfig("hideCursorVk=8\n").hideCursorVk == 0);
    CHECK(ParseConfig("quickZoomVk=8\n").quickZoomVk == 0);
    // A legitimate bind is preserved.
    CHECK(ParseConfig("zoomInVk=33\n").zoomInVk == 33);
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
    CHECK(c.maxLevel == doctest::Approx(12.0));
}
TEST_CASE("numeric fields are clamped to documented ranges") {
    // maxLevel < 1 would invert ZoomController's clamp and disable zoom; must clamp up to 1.0.
    CHECK(ParseConfig("maxLevel=0\n").maxLevel == doctest::Approx(1.0));
    CHECK(ParseConfig("maxLevel=-5\n").maxLevel == doctest::Approx(1.0));
    CHECK(ParseConfig("maxLevel=999\n").maxLevel == doctest::Approx(50.0));   // capped
    // Speeds, accel, ramp, sensitivity, smoothing, sharpness, brightness clamp to their ranges.
    CHECK(ParseConfig("zoomInSpeed=0\n").zoomInSpeed == doctest::Approx(0.25));
    CHECK(ParseConfig("zoomOutSpeed=99\n").zoomOutSpeed == doctest::Approx(4.0));
    CHECK(ParseConfig("smoothZoomAccel=0\n").smoothZoomAccel == doctest::Approx(1.0));
    CHECK(ParseConfig("smoothZoomRamp=-1\n").smoothZoomRamp == doctest::Approx(0.1));
    CHECK(ParseConfig("cursorSensitivity=10\n").cursorSensitivity == doctest::Approx(4.0));
    CHECK(ParseConfig("cursorSmoothing=5\n").cursorSmoothing == doctest::Approx(0.95));
    CHECK(ParseConfig("cursorSmoothing=-1\n").cursorSmoothing == doctest::Approx(0.0));
    CHECK(ParseConfig("sharpness=9\n").sharpness == doctest::Approx(1.0));
    CHECK(ParseConfig("brightness=0\n").brightness == doctest::Approx(0.5));
    // In-range values pass through untouched.
    CHECK(ParseConfig("maxLevel=8\n").maxLevel == doctest::Approx(8.0));
    CHECK(ParseConfig("cursorSmoothing=0.4\n").cursorSmoothing == doctest::Approx(0.4));
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
TEST_CASE("alternate zoom side-buttons default 0 and parse") {
    Config d = ParseConfig("");
    CHECK(d.zoomInButton2 == 0);
    CHECK(d.zoomOutButton2 == 0);
    Config c = ParseConfig("zoomInButton2=2\nzoomOutButton2=1\n");
    CHECK(c.zoomInButton2 == 2);
    CHECK(c.zoomOutButton2 == 1);
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
TEST_CASE("quick-zoom config parses and clamps") {
    Config def = ParseConfig("");
    CHECK(def.quickZoomDefault == doctest::Approx(4.0));
    CHECK(def.quickZoomModifier == "Ctrl");
    CHECK(def.quickZoomHotkeyMode == 0);
    CHECK(def.quickZoomVk == 112);   // F1
    CHECK(def.quickZoomMods == 0);

    Config c = ParseConfig("quickZoomDefault=6.0\nquickZoomModifier=Alt\nquickZoomHotkeyMode=1\nquickZoomVk=120\nquickZoomMods=1\n");
    CHECK(c.quickZoomDefault == doctest::Approx(6.0));
    CHECK(c.quickZoomModifier == "Alt");
    CHECK(c.quickZoomHotkeyMode == 1);
    CHECK(c.quickZoomVk == 120);     // F9
    CHECK(c.quickZoomMods == 1);     // Ctrl

    Config off = ParseConfig("quickZoomModifier=None\n");
    CHECK(off.quickZoomModifier == "None");

    Config hi = ParseConfig("quickZoomDefault=99\n");
    CHECK(hi.quickZoomDefault == doctest::Approx(50.0)); // clamped to max
    Config lo = ParseConfig("quickZoomDefault=0.1\n");
    CHECK(lo.quickZoomDefault == doctest::Approx(1.0));  // clamped to min
}
TEST_CASE("ParseHexColor parses 6-digit hex with and without leading #") {
    float r = -1, g = -1, b = -1;
    CHECK(ParseHexColor("#5b5bd6", r, g, b) == true);
    CHECK(r == doctest::Approx(91.0f / 255.0f));   // 0x5b
    CHECK(g == doctest::Approx(91.0f / 255.0f));   // 0x5b
    CHECK(b == doctest::Approx(214.0f / 255.0f));  // 0xd6

    float r2, g2, b2;
    CHECK(ParseHexColor("ffffff", r2, g2, b2) == true);
    CHECK(r2 == doctest::Approx(1.0f));
    CHECK(g2 == doctest::Approx(1.0f));
    CHECK(b2 == doctest::Approx(1.0f));

    float r3, g3, b3;
    CHECK(ParseHexColor("FF0000", r3, g3, b3) == true);   // uppercase
    CHECK(r3 == doctest::Approx(1.0f));
    CHECK(g3 == doctest::Approx(0.0f));
    CHECK(b3 == doctest::Approx(0.0f));
}
TEST_CASE("outline keys default off with accent color") {
    Config c = ParseConfig("");
    CHECK(c.outline == 0);                  // off by default
    CHECK(c.outlineThickness == 4);
    CHECK(c.outlineColor == "#5b5bd6");     // Wind accent
}
TEST_CASE("outline keys parse and thickness clamps to [1,40]") {
    Config c = ParseConfig("outline=1\noutlineThickness=8\noutlineColor=#ff0000\n");
    CHECK(c.outline == 1);
    CHECK(c.outlineThickness == 8);
    CHECK(c.outlineColor == "#ff0000");
    CHECK(ParseConfig("outlineThickness=0\n").outlineThickness == 1);     // clamp low
    CHECK(ParseConfig("outlineThickness=999\n").outlineThickness == 40);  // clamp high
}
TEST_CASE("ParseHexColor rejects malformed input and leaves outputs untouched") {
    float r = 0.5f, g = 0.5f, b = 0.5f;
    CHECK(ParseHexColor("", r, g, b) == false);
    CHECK(ParseHexColor("#", r, g, b) == false);
    CHECK(ParseHexColor("12345", r, g, b) == false);     // too short
    CHECK(ParseHexColor("1234567", r, g, b) == false);   // too long
    CHECK(ParseHexColor("gggggg", r, g, b) == false);    // non-hex
    CHECK(r == doctest::Approx(0.5f));                    // unchanged on failure
    CHECK(g == doctest::Approx(0.5f));
    CHECK(b == doctest::Approx(0.5f));
}
TEST_CASE("OutlineVisibleAtLevel honors master toggle and low-zoom cutoff") {
    Config c;                       // defaults: outline=0, outlineLowZoomOnly=0, outlineLowZoomMax=2.0
    CHECK(OutlineVisibleAtLevel(c, 1.5) == false);   // master off
    c.outline = 1;
    CHECK(OutlineVisibleAtLevel(c, 1.5) == true);    // on, no cutoff
    CHECK(OutlineVisibleAtLevel(c, 9.0) == true);    // on, cutoff disabled -> any level
    c.outlineLowZoomOnly = 1;                        // cutoff at 2.0
    CHECK(OutlineVisibleAtLevel(c, 1.5) == true);    // below cutoff
    CHECK(OutlineVisibleAtLevel(c, 2.0) == true);    // exactly at cutoff (inclusive)
    CHECK(OutlineVisibleAtLevel(c, 2.5) == false);   // above cutoff
}
TEST_CASE("OutlineIdleAlpha ramps from 1 to 0 across the fade window") {
    CHECK(OutlineIdleAlpha(0.0, 7.0, 0.3) == doctest::Approx(1.0));   // not idle yet
    CHECK(OutlineIdleAlpha(7.0, 7.0, 0.3) == doctest::Approx(1.0));   // at threshold, fade starts
    CHECK(OutlineIdleAlpha(7.15, 7.0, 0.3) == doctest::Approx(0.5));  // ~mid-fade (0.5 within tolerance)
    CHECK(OutlineIdleAlpha(7.3, 7.0, 0.3) == doctest::Approx(0.0));   // fully faded
    CHECK(OutlineIdleAlpha(99.0, 7.0, 0.3) == doctest::Approx(0.0));  // stays faded
    CHECK(OutlineIdleAlpha(6.9, 7.0, 0.0) == doctest::Approx(1.0));   // degenerate fade<=0 -> step
    CHECK(OutlineIdleAlpha(7.0, 7.0, 0.0) == doctest::Approx(0.0));
}
TEST_CASE("OutlineDwellSeconds gates appearance until the band is held for the threshold") {
    const double thr = 1.0;
    double s = 0.0;
    s = OutlineDwellSeconds(true, s, 0.4, thr); CHECK(s == doctest::Approx(0.4)); CHECK(s < thr);  // building
    s = OutlineDwellSeconds(true, s, 0.4, thr); CHECK(s == doctest::Approx(0.8)); CHECK(s < thr);
    s = OutlineDwellSeconds(true, s, 0.4, thr); CHECK(s == doctest::Approx(1.0)); CHECK(s >= thr); // capped, now shows
    s = OutlineDwellSeconds(true, s, 0.4, thr); CHECK(s == doctest::Approx(1.0));                  // stays capped while in-band
    s = OutlineDwellSeconds(false, s, 0.4, thr); CHECK(s == doctest::Approx(0.0)); CHECK(s < thr); // left band -> reset
    // A quick pass-through (total in-band time < threshold) never reaches the gate.
    double q = 0.0;
    q = OutlineDwellSeconds(true, q, 0.3, thr);
    q = OutlineDwellSeconds(false, q, 0.3, thr);   // left before 1s elapsed
    CHECK(q == doctest::Approx(0.0)); CHECK(q < thr);
    // Negative/zero dt never decrements the accumulator.
    CHECK(OutlineDwellSeconds(true, 0.5, -0.2, thr) == doctest::Approx(0.5));
}
TEST_CASE("outline low-zoom + idle keys default and parse with clamps") {
    Config d = ParseConfig("");
    CHECK(d.outlineLowZoomOnly == 0);
    CHECK(d.outlineLowZoomMax  == doctest::Approx(2.0));
    CHECK(d.outlineIdleHide    == 0);
    CHECK(d.outlineIdleSeconds == doctest::Approx(7.0));

    Config c = ParseConfig(
        "outlineLowZoomOnly=1\noutlineLowZoomMax=3.5\noutlineIdleHide=1\noutlineIdleSeconds=10\n");
    CHECK(c.outlineLowZoomOnly == 1);
    CHECK(c.outlineLowZoomMax  == doctest::Approx(3.5));
    CHECK(c.outlineIdleHide    == 1);
    CHECK(c.outlineIdleSeconds == doctest::Approx(10.0));

    // Clamps: outlineLowZoomMax [1.0,50.0]; outlineIdleSeconds [0.5,60.0].
    CHECK(ParseConfig("outlineLowZoomMax=0.2\n").outlineLowZoomMax == doctest::Approx(1.0));
    CHECK(ParseConfig("outlineLowZoomMax=99\n").outlineLowZoomMax  == doctest::Approx(50.0));
    CHECK(ParseConfig("outlineIdleSeconds=0\n").outlineIdleSeconds == doctest::Approx(0.5));
    CHECK(ParseConfig("outlineIdleSeconds=120\n").outlineIdleSeconds == doctest::Approx(60.0));
}
