#include "doctest.h"
#include "../src/config.h"
#include "../src/config_ui/ini_edit.h"
using namespace wind;

TEST_CASE("defaults when text is empty") {
    Config c = ParseConfig("");
    CHECK(c.zoomInButton  == 0);   // shipped unbound (onboarding captures it)
    CHECK(c.zoomOutButton == 0);   // shipped unbound
    CHECK(c.maxLevel == doctest::Approx(12.0));
    CHECK(c.diagnostics == 0);
    // Ramp step cap ships ON (issue #219): uncapped, ~15% of high-level zoom-ins stall 35-43ms
    // then snap 1.2-1.9 levels; capped at 2.5%/tick every measured ramp was even. 0 would
    // regress the acrylic-window hitch reproduced on the test machine.
    CHECK(c.txMaxStepPct == 25);
}
TEST_CASE("StripUiOnlyKeys drops exactly the UI-owned lines (theme toggle must not hot-reload the core)") {
    const std::string ini =
        "maxLevel=12\nuiTheme=dark\n  showAdvanced=1\nonboarded=1\nzoomInSpeed=2\nprofile=Default\n";
    const std::string s = wind::StripUiOnlyKeys(ini);
    CHECK(s == "maxLevel=12\nzoomInSpeed=2\nprofile=Default\n");   // profile STAYS: mirroring needs it
    // A theme flip alone produces an identical stripped form - the skip condition.
    CHECK(wind::StripUiOnlyKeys("a=1\nuiTheme=light\n") == wind::StripUiOnlyKeys("a=1\nuiTheme=dark\n"));
    // A key that merely STARTS like a UI key is kept.
    CHECK(wind::StripUiOnlyKeys("uiThemeX=1\n") == "uiThemeX=1\n");
}

TEST_CASE("uiTheme is an ignored legacy key: uiTheme=light or auto parses like no key at all (#324)") {
    const Config base = ParseConfig("maxLevel=7\n");
    for (const char* v : { "light", "dark", "auto", "" }) {
        const Config c = ParseConfig(std::string("uiTheme=") + v + "\nmaxLevel=7\n");
        CHECK(c.maxLevel == base.maxLevel);
        CHECK(c.model == base.model);
    }
    // Old inis keep working: the key stays a UI-only line the core never reloads for.
    CHECK(wind::StripUiOnlyKeys("a=1\nuiTheme=light\n") == "a=1\n");
}

TEST_CASE("parses renderer knobs") {
    Config c = ParseConfig(
        "cursorSensitivity=1.5\ncursorConstantSize=1\nbilinear=1\n");
    CHECK(c.cursorSensitivity == doctest::Approx(1.5));
    CHECK(c.cursorConstantSize == 1);
    CHECK(c.bilinear == 1);
}
// Issue #253: every ini written before 0.6.3 carries an explicit cursorScaleWithZoom=0 from the
// default template (no UI ever exposed it, so nobody chose it), which pinned the render engine's
// cursor at desktop size - a tiny pointer on every fresh install. The key is retired so the
// update fixes those installs; cursorConstantSize is the opt-in for the old look.
TEST_CASE("legacy cursorScaleWithZoom=0 no longer pins a constant-size cursor") {
    Config c = ParseConfig("cursorScaleWithZoom=0\n");
    CHECK(c.cursorConstantSize == 0);
}
TEST_CASE("renderer knobs have sane defaults") {
    Config c = ParseConfig("");
    CHECK(c.cursorSensitivity == doctest::Approx(1.0));
    CHECK(c.cursorConstantSize == 0);          // cursor grows with the zoom, like transform (issue #253)
    CHECK(c.bilinear == 1);
    CHECK(c.sharpness == doctest::Approx(0.0));   // off by default
    CHECK(c.panGlideMaxPx == 0);
    CHECK(c.zorderBand == 0);                  // shipped unbanded (issue #162): band 16 sat UNDER the
                                               // Snipping Tool overlay, which cost the cursor entirely
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
TEST_CASE("keyboard zoom ships unbound (onboarding captures it); recenter unbound; all parseable") {
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
TEST_CASE("launchQuiesce defaults on and is a strict 0/1 switch (issue #247)") {
    // Shipped default must stay ON: it is the guard for the #187 dwmcore crash.
    CHECK(ParseConfig("").launchQuiesce == 1);
    CHECK(ParseConfig("launchQuiesce=0\n").launchQuiesce == 0);
    CHECK(ParseConfig("launchQuiesce=1\n").launchQuiesce == 1);
    // Any non-zero value is just "on"; a stray -1 or 7 must not become a third state.
    CHECK(ParseConfig("launchQuiesce=7\n").launchQuiesce == 1);
    CHECK(ParseConfig("launchQuiesce=-1\n").launchQuiesce == 1);
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
TEST_CASE("parses z-order band, brightness; the old cursorSmoothing and panGlideMs are ignored") {
    Config c = ParseConfig("panGlideMs=150\ncursorSmoothing=0.7\nzorderBand=16\nbrightness=0.85\n");
    CHECK(c.panGlideMaxPx == 0);
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
    CHECK(ParseConfig("maxLevel=999\n").maxLevel == doctest::Approx(30.0));   // capped (#429)
    CHECK(ParseConfig("maxLevel=50\n").maxLevel == doctest::Approx(30.0));
    // Speeds, accel, ramp, sensitivity, smoothing, sharpness, brightness clamp to their ranges.
    CHECK(ParseConfig("zoomInSpeed=0\n").zoomInSpeed == doctest::Approx(0.25));
    CHECK(ParseConfig("zoomOutSpeed=99\n").zoomOutSpeed == doctest::Approx(4.0));
    CHECK(ParseConfig("smoothZoomAccel=0\n").smoothZoomAccel == doctest::Approx(1.0));
    CHECK(ParseConfig("smoothZoomRamp=-1\n").smoothZoomRamp == doctest::Approx(0.1));
    CHECK(ParseConfig("cursorSensitivity=10\n").cursorSensitivity == doctest::Approx(4.0));
    CHECK(ParseConfig("panGlideMaxPx=9999\n").panGlideMaxPx == 400);
    CHECK(ParseConfig("sharpness=9\n").sharpness == doctest::Approx(1.0));
    CHECK(ParseConfig("brightness=0\n").brightness == doctest::Approx(0.5));
    // In-range values pass through untouched.
    CHECK(ParseConfig("maxLevel=8\n").maxLevel == doctest::Approx(8.0));
    CHECK(ParseConfig("panGlideMaxPx=40\n").panGlideMaxPx == 40);
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
    CHECK(hi.quickZoomDefault == doctest::Approx(30.0)); // clamped to max
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
TEST_CASE("game perf keys (issue #148) default and parse with clamps") {
    Config d = ParseConfig("");
    CHECK(d.lowGpuPriority == 0);   // OFF by default: a saturated game can starve the zoomed view
    CHECK(d.gameCrop == 1);         // on by default: crop is always safe on full-screen repaints
    CHECK(d.gameFpsCap == 0);       // off by default: opt-in second lever

    Config c = ParseConfig("lowGpuPriority=1\ngameCrop=0\ngameFpsCap=72\n");
    CHECK(c.lowGpuPriority == 1);
    CHECK(c.gameCrop == 0);
    CHECK(c.gameFpsCap == 72);

    // gameFpsCap clamps to [0, 240]; negative means off.
    CHECK(ParseConfig("gameFpsCap=-5\n").gameFpsCap == 0);
    CHECK(ParseConfig("gameFpsCap=999\n").gameFpsCap == 240);
    // Bad values keep defaults.
    Config b = ParseConfig("lowGpuPriority=x\ngameFpsCap=x\n");
    CHECK(b.lowGpuPriority == 0);
    CHECK(b.gameFpsCap == 0);
}
TEST_CASE("gpuPriority tri-state parses, clamps, and folds the legacy alias") {
    CHECK(ParseConfig("").gpuPriority == 0);                    // default normal
    CHECK(ParseConfig("gpuPriority=1\n").gpuPriority == 1);
    CHECK(ParseConfig("gpuPriority=-1\n").gpuPriority == -1);
    CHECK(ParseConfig("gpuPriority=7\n").gpuPriority == 1);     // clamped to tri-state
    CHECK(ParseConfig("gpuPriority=-9\n").gpuPriority == -1);

    // EffectiveGpuPriority: explicit gpuPriority wins; legacy lowGpuPriority=1 means low.
    CHECK(EffectiveGpuPriority(ParseConfig("")) == 0);
    CHECK(EffectiveGpuPriority(ParseConfig("gpuPriority=1\n")) == 1);
    CHECK(EffectiveGpuPriority(ParseConfig("lowGpuPriority=1\n")) == -1);
    CHECK(EffectiveGpuPriority(ParseConfig("gpuPriority=1\nlowGpuPriority=1\n")) == 1);
}

TEST_CASE("the high-res/MPO option is atomic at restart (issue #242)") {
    // Args: (iniValue, mpoDisabledAtBoot, mpoDisabledInRegistry, tdrTest).
    // STEADY STATES (registry == boot) run the ini value, except the TDR combo:
    CHECK(EffectiveSamplingMode(1, false, false, 0) == 1);  // smooth + MPO on: the shipped pair
    CHECK(EffectiveSamplingMode(0, true,  true,  0) == 0);  // crisp + MPO off: the shipped pair
    CHECK(EffectiveSamplingMode(1, true,  true,  0) == 1);  // smooth + MPO off: legacy, safe, kept
    CHECK(EffectiveSamplingMode(0, false, false, 0) == 1);  // crisp + MPO on (profile switch,
                                                            //   hand edit): the TDR combo -> smooth
    // RESTART PENDING (registry != boot): the BOOT state's look holds in BOTH directions, so
    // flipping the toggle changes nothing on screen until the restart lands.
    CHECK(EffectiveSamplingMode(0, false, true,  0) == 1);  // turned high-res OFF: stay smooth
    CHECK(EffectiveSamplingMode(1, true,  false, 0) == 0);  // turned high-res ON: stay crisp
    // The field harness must be able to repro nearest+MPO deliberately.
    CHECK(EffectiveSamplingMode(0, false, false, 2) == 0);
    CHECK(EffectiveSamplingMode(0, false, true,  4) == 0);
    // MPO nearest guard (issue #369): nearest may run on an MPO boot when the guard is on; a pending
    // restart still holds the boot look.
    CHECK(EffectiveSamplingMode(0, false, false, 0, true) == 0);
    CHECK(EffectiveSamplingMode(0, false, true,  0, true) == 0);   // no restart-pending hold
    CHECK(EffectiveSamplingMode(1, true,  false, 0, true) == 1);
    CHECK(ParseConfig("").mpoNearestGuard == 1);
    CHECK(ParseConfig("mpoNearestGuard=0\n").mpoNearestGuard == 0);
    CHECK(ParseConfig("mpoNearestGuard=1\n").mpoNearestGuard == 1);
}

TEST_CASE("cursorBandAuto defaults on and parses off (issue #269)") {
    CHECK(ParseConfig("").cursorBandAuto == 1);
    CHECK(ParseConfig("cursorBandAuto=0\n").cursorBandAuto == 0);
    CHECK(ParseConfig("cursorBandAuto=1\n").cursorBandAuto == 1);
}

TEST_CASE("the transform engine is the desktop default (issue #271)") {
    CHECK(ParseConfig("").desktopTransform == 1);
    CHECK(ParseConfig("desktopTransform=0\n").desktopTransform == 0);
}

TEST_CASE("the first-run ini template parses to the struct defaults (issue #274)") {
    // LoadConfig writes DefaultIniText() on first run and runs with ParseConfig of it; this pins
    // that doing so changes nothing versus Config{}. Generated from the template's own keys, so a
    // default changed in only one of the two places fails here instead of shipping.
    const Config d{};
    const Config t = ParseConfig(DefaultIniText());
    CHECK(t.zoomInButton == d.zoomInButton);
    CHECK(t.zoomInVk == d.zoomInVk);
    CHECK(t.zoomInMods == d.zoomInMods);
    CHECK(t.zoomInButton2 == d.zoomInButton2);
    CHECK(t.zoomInVk2 == d.zoomInVk2);
    CHECK(t.hideCursorVk == d.hideCursorVk);
    CHECK(t.recenterVk == d.recenterVk);
    CHECK(t.cursorLockVk == d.cursorLockVk);
    CHECK(t.maxLevel == doctest::Approx(d.maxLevel));
    CHECK(t.zoomInSpeed == doctest::Approx(d.zoomInSpeed));
    CHECK(t.smoothZoom == d.smoothZoom);
    CHECK(t.smoothZoomAccel == doctest::Approx(d.smoothZoomAccel));
    CHECK(t.smoothZoomRamp == doctest::Approx(d.smoothZoomRamp));
    CHECK(t.quickZoomHotkeyMode == d.quickZoomHotkeyMode);
    CHECK(t.quickZoomModifier == d.quickZoomModifier);
    CHECK(t.quickZoomVk == d.quickZoomVk);
    CHECK(t.quickZoomDefault == doctest::Approx(d.quickZoomDefault));
    CHECK(t.vsync == d.vsync);
    CHECK(t.dwmFlush == d.dwmFlush);
    CHECK(t.diagnostics == d.diagnostics);
    CHECK(t.cursorSensitivity == doctest::Approx(d.cursorSensitivity));
    CHECK(t.panGlideMaxPx == d.panGlideMaxPx);
    CHECK(t.cursorConstantSize == d.cursorConstantSize);
    CHECK(t.trayPinned == d.trayPinned);
    CHECK(t.cursorVisibility == d.cursorVisibility);
    CHECK(t.bilinear == d.bilinear);
    CHECK(t.sharpness == doctest::Approx(d.sharpness));
    CHECK(t.zorderBand == d.zorderBand);
    CHECK(t.cursorBandAuto == d.cursorBandAuto);
    CHECK(t.brightness == doctest::Approx(d.brightness));
    CHECK(t.hdrTonemap == d.hdrTonemap);
    CHECK(t.model == d.model);
    CHECK(t.transformExclude == d.transformExclude);
    CHECK(t.renderExclude == d.renderExclude);
    CHECK(t.engineGame == d.engineGame);
    CHECK(t.engineAcrylic == d.engineAcrylic);
    CHECK(t.engineDesktop == d.engineDesktop);
    CHECK(t.engineOther == d.engineOther);
    CHECK(t.noSwallowApps == d.noSwallowApps);
    CHECK(t.multiMonitor == d.multiMonitor);
    CHECK(t.cropCapture == d.cropCapture);
    CHECK(t.gpuPriority == d.gpuPriority);
    CHECK(t.gameCrop == d.gameCrop);
    CHECK(t.gameFpsCap == d.gameFpsCap);
    CHECK(t.outline == d.outline);
    CHECK(t.outlineThickness == d.outlineThickness);
    CHECK(t.outlineColor == d.outlineColor);
    CHECK(t.outlineLowZoomOnly == d.outlineLowZoomOnly);
    CHECK(t.outlineLowZoomMax == doctest::Approx(d.outlineLowZoomMax));
    CHECK(t.outlineIdleHide == d.outlineIdleHide);
    CHECK(t.outlineIdleSeconds == doctest::Approx(d.outlineIdleSeconds));
    CHECK(t.onboarded == d.onboarded);
    CHECK(t.trackCaret == d.trackCaret);
    CHECK(t.trackFocus == d.trackFocus);
    CHECK(t.trackAlign == d.trackAlign);
    CHECK(t.mouseAlign == d.mouseAlign);
    CHECK(t.trackGlideMs == d.trackGlideMs);
    CHECK(t.trackMarginPct == d.trackMarginPct);
}

TEST_CASE("tracking settings: defaults and parsing (issue #276)") {
    Config d = ParseConfig("");
    CHECK(d.trackCaret == 1);
    CHECK(d.trackFocus == 0);
    CHECK(d.trackAlign == 0);
    CHECK(d.mouseAlign == 0);
    CHECK(d.trackGlideMs == 200);
    CHECK(d.trackMarginPct == 15);
    CHECK(d.trackLog == 0);
    Config c = ParseConfig("trackCaret=0\ntrackFocus=1\ntrackAlign=1\nmouseAlign=1\n"
                           "trackGlideMs=90\ntrackMarginPct=20\ntrackLog=1\n");
    CHECK(c.trackCaret == 0); CHECK(c.trackFocus == 1); CHECK(c.trackAlign == 1);
    CHECK(c.mouseAlign == 1); CHECK(c.trackGlideMs == 90); CHECK(c.trackMarginPct == 20);
    CHECK(c.trackLog == 1);
}

TEST_CASE("colour keys: defaults, parse and clamps (#288)") {
    Config d = ParseConfig("");
    CHECK(d.colorWarmPct == 0); CHECK(d.colorDimPct == 100);
    Config c = ParseConfig("colorWarmPct=80\ncolorDimPct=60\n");
    CHECK(c.colorWarmPct == 80); CHECK(c.colorDimPct == 60);
    Config x = ParseConfig("colorWarmPct=-4\ncolorDimPct=-5\n");
    CHECK(x.colorWarmPct == 0); CHECK(x.colorDimPct == 1);
    CHECK(ParseConfig("colorWarmPct=150\n").colorWarmPct == 100);
    // Keys of the dropped controls are ignored, not errors.
    Config old = ParseConfig("colorFilter=4\ncolorAt1x=0\ncolorToggleVk=67\n");
    CHECK(old.colorWarmPct == 0); CHECK(old.colorDimPct == 100);
}

TEST_CASE("unsafe binds in an ini read as unbound; safe ones survive (#285)") {
    Config c = ParseConfig("zoomInVk=65\nzoomOutVk=34\nzoomInVk2=50\nzoomInMods2=3\nzoomOutVk2=115\nzoomOutMods2=2\n"
                           "hideCursorVk=112\nhideCursorMods=1\nrecenterVk=82\ncursorLockVk=113\n");
    CHECK(c.zoomInVk == 0);                              // A alone
    CHECK(c.zoomOutVk == 34);                            // PageDown alone is fine
    CHECK(c.zoomInVk2 == 0); CHECK(c.zoomInMods2 == 0);  // Ctrl+Alt+2 = AltGr @
    CHECK(c.zoomOutVk2 == 0); CHECK(c.zoomOutMods2 == 0);// Alt+F4
    CHECK(c.hideCursorVk == 112); CHECK(c.hideCursorMods == 1);
    CHECK(c.recenterVk == 0);                            // R alone
    CHECK(c.cursorLockVk == 113);                        // F2 alone
}
TEST_CASE("click and wheel binds need a modifier; Ctrl+wheel is fine, Ctrl+click and Shift are not (#285, #295)") {
    Config c = ParseConfig("zoomInButton=3\nzoomInButtonMods=3\nzoomOutButton=4\nzoomOutButtonMods=1\n"
                           "zoomInButton2=2\nzoomWheelMods=2\n");
    CHECK(c.zoomInButton == 3); CHECK(c.zoomInButtonMods == 3);    // Ctrl+Alt+left click
    CHECK(c.zoomOutButton == 0); CHECK(c.zoomOutButtonMods == 0);  // Ctrl+right click: refused
    CHECK(c.zoomInButton2 == 2);                                    // side button alone: fine
    CHECK(c.zoomWheelMods == 2);
    CHECK(ParseConfig("zoomWheelMods=1\n").zoomWheelMods == 1);     // Ctrl+wheel: Wind zooms instead
    CHECK(ParseConfig("zoomWheelMods=4\n").zoomWheelMods == 0);     // Shift+wheel
    CHECK(ParseConfig("zoomInButton=5\n").zoomInButton == 0);       // bare middle click
}

TEST_CASE("pan keys ship unbound (#307); Ctrl+Alt+arrows bind; unsafe binds and speeds are sanitised (#287)") {
    Config d = ParseConfig("");
    CHECK(d.panLeftVk == 0); CHECK(d.panRightVk == 0); CHECK(d.panUpVk == 0); CHECK(d.panDownVk == 0);
    Config wm = ParseConfig("panLeftVk=37\npanLeftMods=3\npanDownVk=40\npanDownMods=7\n");
    CHECK(wm.panLeftVk == 37); CHECK(wm.panLeftMods == 3);        // Ctrl+Alt+Left
    CHECK(wm.panDownVk == 40); CHECK(wm.panDownMods == 7);        // Ctrl+Alt+Shift+Down
    CHECK(d.panSpeed == doctest::Approx(1.0));
    CHECK(ParseConfig("panSpeed=9\n").panSpeed == doctest::Approx(4.0));
    CHECK(ParseConfig("panSpeed=0\n").panSpeed == doctest::Approx(0.25));
    Config a = ParseConfig("panLeftVk=65\npanLeftMods=0\n");   // bare A: refused
    CHECK(a.panLeftVk == 0); CHECK(a.panLeftMods == 0);
    Config u = ParseConfig("panUpVk=33\npanUpMods=0\n");       // PageUp alone: fine
    CHECK(u.panUpVk == 33); CHECK(u.panUpMods == 0);
    Config off = ParseConfig("panDownVk=0\n");                         // unbound on purpose
    CHECK(off.panDownVk == 0);
}

TEST_CASE("every keybind takes two or three modifiers plus one key (#307)") {
    Config c = ParseConfig("zoomInVk=112\nzoomInMods=7\n"            // Ctrl+Alt+Shift+F1
                           "zoomOutVk=34\nzoomOutMods=3\n"           // Ctrl+Alt+PageDown
                           "hideCursorVk=113\nhideCursorMods=11\n"   // Ctrl+Alt+Win+F2
                           "quickZoomVk=114\nquickZoomMods=5\n"      // Ctrl+Shift+F3
                           "cursorLockVk=115\ncursorLockMods=7\n"    // Ctrl+Alt+Shift+F4
                           "recenterVk=36\nrecenterMods=6\n"         // Alt+Shift+Home
                           "zoomWheelMods=7\n"                         // Ctrl+Alt+Shift+wheel
                           "zoomInButton=3\nzoomInButtonMods=7\n");  // Ctrl+Alt+Shift+left click
    CHECK(c.zoomInVk == 112); CHECK(c.zoomInMods == 7);
    CHECK(c.zoomOutVk == 34); CHECK(c.zoomOutMods == 3);
    CHECK(c.hideCursorVk == 113); CHECK(c.hideCursorMods == 11);
    CHECK(c.quickZoomVk == 114); CHECK(c.quickZoomMods == 5);
    CHECK(c.cursorLockVk == 115); CHECK(c.cursorLockMods == 7);
    CHECK(c.recenterVk == 36); CHECK(c.recenterMods == 6);
    CHECK(c.zoomWheelMods == 7);
    CHECK(c.zoomInButton == 3); CHECK(c.zoomInButtonMods == 7);
    // A modifier as the key is still refused everywhere (Ctrl+Alt with Shift as the "key").
    Config m = ParseConfig("cursorLockVk=16\ncursorLockMods=3\n");
    CHECK(m.cursorLockVk == 0); CHECK(m.cursorLockMods == 0);
    // A letter alone for Inspect still reads as unbound (it would stop you typing it).
    CHECK(ParseConfig("cursorLockVk=82\n").cursorLockVk == 0);
}

TEST_CASE("zoomTrace is off by default and parses (#310)") {
    CHECK(ParseConfig("").zoomTrace == 0);
    CHECK(ParseConfig("zoomTrace=1\n").zoomTrace == 1);
}

// ---- #318: wheel zoom binds, the zoomWheelMods migration, extra-key switches, uiPalette ----
TEST_CASE("wheel up and down are zoom button codes 6/7 and follow the wheel rule (#318)") {
    Config c = ParseConfig("zoomInButton=6\nzoomInButtonMods=1\nzoomOutButton=7\nzoomOutButtonMods=3\n"
                           "zoomInButton2=2\nzoomOutButton2=7\nzoomOutButton2Mods=0\n");
    CHECK(c.zoomInButton == 6);  CHECK(c.zoomInButtonMods == 1);    // Ctrl + wheel up
    CHECK(c.zoomOutButton == 7); CHECK(c.zoomOutButtonMods == 3);   // Ctrl+Alt + wheel down
    CHECK(c.zoomInButton2 == 2);                                    // a side button next to it
    CHECK(c.zoomOutButton2 == 0);                                   // wheel with no modifier: refused
    Config s = ParseConfig("zoomInButton=6\nzoomInButtonMods=4\nzoomOutButton=8\nzoomOutButtonMods=3\n");
    CHECK(s.zoomInButton == 0);                                     // Shift alone: refused
    CHECK(s.zoomOutButton == 0);                                    // 8 is not a code
}
TEST_CASE("zoomWheelMods migrates once into free zoom slots (#318)") {
    const auto m = MigrateWheelMods("maxLevel=8\nzoomWheelMods=3\nzoomInButton=2\n");
    CHECK(m.changed); CHECK_FALSE(m.blocked);
    const Config c = ParseConfig(m.text);
    CHECK(c.zoomInButton == 2);                                     // the existing bind stays first
    CHECK(c.zoomInButton2 == 6);  CHECK(c.zoomInButton2Mods == 3);  // wheel up + Ctrl+Alt in the free slot
    CHECK(c.zoomOutButton == 7);  CHECK(c.zoomOutButtonMods == 3);  // wheel down + the same mods
    CHECK(c.zoomWheelMods == 0);
    CHECK(m.text.find("maxLevel=8") != std::string::npos);          // other lines untouched
    // Idempotent: a second pass has nothing to do.
    const auto again = MigrateWheelMods(m.text);
    CHECK_FALSE(again.changed); CHECK(again.text == m.text);
}
TEST_CASE("zoomWheelMods stays when a direction has no free slot (#318)") {
    const std::string ini = "zoomWheelMods=1\nzoomOutButton=2\nzoomOutButton2=1\n";
    const auto m = MigrateWheelMods(ini);
    CHECK(m.blocked); CHECK_FALSE(m.changed); CHECK(m.text == ini);
    CHECK(ParseConfig(m.text).zoomWheelMods == 1);                  // the core keeps honouring it
    const auto full = MigrateWheelMods("zoomWheelMods=1\nzoomInButton=2\nzoomInButton2=1\n");
    CHECK(full.blocked); CHECK_FALSE(full.changed);
}
TEST_CASE("migration leaves nothing to do without a wheel setting, and drops an unsafe one (#318)") {
    CHECK_FALSE(MigrateWheelMods("maxLevel=8\n").changed);
    CHECK_FALSE(MigrateWheelMods("zoomWheelMods=0\n").changed);
    const auto bad = MigrateWheelMods("zoomWheelMods=4\n");        // Shift alone never bound
    CHECK(bad.changed); CHECK_FALSE(bad.blocked);
    CHECK(ParseConfig(bad.text).zoomInButton == 0);
    CHECK(ReadIniValues(bad.text)["zoomWheelMods"] == "0");
}
TEST_CASE("extra-key switches default on; off clears the parsed bind but not the ini (#318)") {
    const Config d = ParseConfig("panLeftVk=37\npanLeftMods=3\nhideCursorVk=112\ncursorLockVk=113\n");
    CHECK(d.panKeysOn == 1); CHECK(d.hideCursorOn == 1); CHECK(d.cursorLockOn == 1);
    CHECK(d.panLeftVk == 37); CHECK(d.hideCursorVk == 112); CHECK(d.cursorLockVk == 113);
    const std::string ini = "panLeftVk=37\npanLeftMods=3\npanRightVk=39\npanRightMods=3\npanUpVk=38\npanUpMods=3\n"
                            "panDownVk=40\npanDownMods=3\nhideCursorVk=112\nhideCursorMods=1\ncursorLockVk=113\n"
                            "recenterVk=36\nrecenterMods=2\n";
    const Config off = ParseConfig(ini + "panKeysOn=0\nhideCursorOn=0\ncursorLockOn=0\n");
    CHECK(off.panKeysOn == 0); CHECK(off.hideCursorOn == 0); CHECK(off.cursorLockOn == 0);
    CHECK(off.panLeftVk == 0); CHECK(off.panRightVk == 0); CHECK(off.panUpVk == 0); CHECK(off.panDownVk == 0);
    CHECK(off.panLeftMods == 0);
    CHECK(off.hideCursorVk == 0); CHECK(off.hideCursorMods == 0);
    CHECK(off.cursorLockVk == 0);
    CHECK(off.recenterVk == 36);                                    // not an extra key: unaffected
    // Each switch is independent.
    const Config one = ParseConfig(ini + "hideCursorOn=0\n");
    CHECK(one.hideCursorVk == 0); CHECK(one.panLeftVk == 37); CHECK(one.cursorLockVk == 113);
    // Any non-zero value reads as on; the key may sit before or after the binding.
    CHECK(ParseConfig("panKeysOn=2\npanLeftVk=37\npanLeftMods=3\n").panLeftVk == 37);
    CHECK(ParseConfig("cursorLockOn=0\ncursorLockVk=113\n").cursorLockVk == 0);
}
TEST_CASE("uiPalette is UI-only: stripped from the core text, unknown reads grey (#318)") {
    CHECK(StripUiOnlyKeys("a=1\nuiPalette=ember\nb=2\n") == "a=1\nb=2\n");
    CHECK(StripUiOnlyKeys("a=1\nuiPalette=ember\n") == StripUiOnlyKeys("a=1\nuiPalette=hicon\n"));
    CHECK(StripUiOnlyKeys("uiPaletteX=1\n") == "uiPaletteX=1\n");
    int n = 0; const char* const* ids = UiPaletteIds(n);
    CHECK(n == 4); CHECK(std::string(ids[0]) == "grey");
    for (int i = 0; i < n; ++i) CHECK(NormalizeUiPalette(ids[i]) == ids[i]);
    CHECK(NormalizeUiPalette("") == "grey");
    CHECK(NormalizeUiPalette("neon") == "grey");
    CHECK(NormalizeUiPalette("b1_hicon") == "grey");                // old mockup ids are not ini values
    for (const char* gone : { "cyber", "mono", "slate", "carbon" }) CHECK(NormalizeUiPalette(gone) == "grey");   // removed themes
    CHECK(std::string(ids[3]) == "hicon");                          // High contrast is always last
    CHECK(NormalizeUiPalette(" ocean ") == "ocean");
}
TEST_CASE("the first-run ini carries the new keys and parses to the struct defaults (#318)") {
    const Config t = ParseConfig(DefaultIniText());
    CHECK(t.panKeysOn == 1); CHECK(t.hideCursorOn == 1); CHECK(t.cursorLockOn == 1);
    CHECK(t.zoomWheelMods == 0);
}

TEST_CASE("txSamplingMode folds the nearest aliases 2..4 to 0 (review 2026-10-09 #23)") {
    CHECK(ParseConfig("txSamplingMode=0\n").txSamplingMode == 0);
    CHECK(ParseConfig("txSamplingMode=1\n").txSamplingMode == 1);
    CHECK(ParseConfig("txSamplingMode=-1\n").txSamplingMode == -1);   // leave DWM alone stays valid
    CHECK(ParseConfig("txSamplingMode=2\n").txSamplingMode == 0);
    CHECK(ParseConfig("txSamplingMode=4\n").txSamplingMode == 0);
    CHECK(ParseConfig("txSamplingMode=99\n").txSamplingMode == 0);
    // Folded to 0, the MPO guard sees an MPO-enabled boot's nearest and swaps in smooth.
    CHECK(EffectiveSamplingMode(ParseConfig("txSamplingMode=3\n").txSamplingMode, false, false, 0) == 1);
}

TEST_CASE("ParseConfig rejects NaN, infinity, hex and trailing junk, and keeps the default (review 2026-10-09 #72)") {
    CHECK(ParseConfig("maxLevel=nan\n").maxLevel == doctest::Approx(12.0));
    CHECK(ParseConfig("maxLevel=inf\n").maxLevel == doctest::Approx(12.0));
    CHECK(ParseConfig("maxLevel=0x10\n").maxLevel == doctest::Approx(12.0));
    CHECK(ParseConfig("maxLevel=8abc\n").maxLevel == doctest::Approx(12.0));
    CHECK(ParseConfig("txMaxStepPct=0x10\n").txMaxStepPct == 25);
    CHECK(ParseConfig("txMaxStepPct=7junk\n").txMaxStepPct == 25);
    CHECK(ParseConfig("maxLevel=8\n").maxLevel == doctest::Approx(8.0));
    CHECK(ParseConfig("maxLevel=8.5\n").maxLevel == doctest::Approx(8.5));
    CHECK(ParseConfig("txMaxStepPct=40\n").txMaxStepPct == 40);
    CHECK(ParseConfig("txMaxStepPct=10.0\n").txMaxStepPct == 10);   // a float written for an integer key still reads
}
TEST_CASE("ParseConfig strips a UTF-8 BOM so the first key is not lost (review #72)") {
    CHECK(ParseConfig("\xEF\xBB\xBFmaxLevel=8\n").maxLevel == doctest::Approx(8.0));
}
TEST_CASE("ParseConfig clamps trackGlideMs and txMaxStepPct (review #72)") {
    CHECK(ParseConfig("trackGlideMs=-50\n").trackGlideMs == 0);
    CHECK(ParseConfig("trackGlideMs=999999\n").trackGlideMs == 5000);
    CHECK(ParseConfig("txMaxStepPct=-5\n").txMaxStepPct == 0);
    CHECK(ParseConfig("txMaxStepPct=99999\n").txMaxStepPct == 1000);
}
TEST_CASE("uiHighResNoticeOff is UI-only: never reloads the core and never travels with a profile (#441)") {
    CHECK(StripUiOnlyKeys("a=1\nuiHighResNoticeOff=0\nb=2\n") == "a=1\nb=2\n");
    CHECK(StripUiOnlyKeys("a=1\nuiHighResNoticeOff=0\n") == StripUiOnlyKeys("a=1\nuiHighResNoticeOff=1\n"));
    CHECK(StripUiOnlyKeys("uiHighResNoticeOffX=1\n") == "uiHighResNoticeOffX=1\n");
}
TEST_CASE("trayPinned ships off, clamps to 0..1 and is UI-only: never reloads the core (#436)") {
    CHECK(ParseConfig("").trayPinned == 0);
    CHECK(ParseConfig("trayPinned=0\n").trayPinned == 0);
    CHECK(ParseConfig("trayPinned=1\n").trayPinned == 1);
    CHECK(ParseConfig("trayPinned=7\n").trayPinned == 1);
    CHECK(ParseConfig("trayPinned=-3\n").trayPinned == 0);
    CHECK(ParseConfig("trayPinned=abc\n").trayPinned == 0);   // strict parser: junk keeps the default
    CHECK(StripUiOnlyKeys("a=1\ntrayPinned=0\nb=2\n") == "a=1\nb=2\n");
    CHECK(StripUiOnlyKeys("a=1\ntrayPinned=0\n") == StripUiOnlyKeys("a=1\ntrayPinned=1\n"));
    CHECK(StripUiOnlyKeys("trayPinnedX=1\n") == "trayPinnedX=1\n");
}
