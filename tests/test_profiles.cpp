#include "doctest.h"
#include "../src/profiles.h"
#include "../src/config_ui/ini_edit.h"
#include "../src/config.h"
using namespace wind;

TEST_CASE("global keys are exactly profile/onboarded/uiTheme/uiPalette/showAdvanced") {
    CHECK(IsGlobalProfileKey("profile"));
    CHECK(IsGlobalProfileKey("onboarded"));
    CHECK(IsGlobalProfileKey("uiTheme"));
    CHECK(IsGlobalProfileKey("showAdvanced"));
    CHECK(IsGlobalProfileKey("uiPalette"));   // #318: the built-in theme never travels with a profile
    CHECK_FALSE(IsGlobalProfileKey("panKeysOn"));   // the extra-key switches are per profile
    CHECK_FALSE(IsGlobalProfileKey("hideCursorOn"));
    CHECK_FALSE(IsGlobalProfileKey("cursorLockOn"));
    CHECK_FALSE(IsGlobalProfileKey("model"));
    CHECK_FALSE(IsGlobalProfileKey("zoomInVk"));
    CHECK_FALSE(IsGlobalProfileKey("maxLevel"));
}

TEST_CASE("profile name validation") {
    CHECK(ProfileNameError("Gaming") == "");
    CHECK(ProfileNameError("Desktop 4K") == "");
    CHECK(ProfileNameError("") != "");
    CHECK(ProfileNameError("a/b") != "");
    CHECK(ProfileNameError("a:b") != "");
    CHECK(ProfileNameError("a?b") != "");
    CHECK(ProfileNameError(" lead") != "");
    CHECK(ProfileNameError("trail ") != "");
    CHECK(ProfileNameError("dot.") != "");
    CHECK(ProfileNameError("CON") != "");
    CHECK(ProfileNameError("com3") != "");
    CHECK(ProfileNameError(std::string(41, 'x')) != "");
    CHECK(ProfileNameError(std::string(40, 'x')) == "");
    CHECK(ProfileNameError(std::string("a\tb")) != "");   // control char
}

TEST_CASE("name-taken is case-insensitive") {
    std::vector<std::string> names{"Default", "Gaming"};
    CHECK(ProfileNameTaken("gaming", names));
    CHECK(ProfileNameTaken("DEFAULT", names));
    CHECK_FALSE(ProfileNameTaken("Reading", names));
}

TEST_CASE("MakeProfileText strips global keys, keeps everything else verbatim") {
    const std::string live =
        "; a comment\n"
        "maxLevel=8.0\n"
        "profile=Default\n"
        "onboarded=1\n"
        "uiTheme=dark\n"
        "showAdvanced=1\n"
        "zoomInVk=33\n";
    const std::string p = MakeProfileText(live);
    auto v = ReadIniValues(p);
    CHECK(v.count("maxLevel") == 1);
    CHECK(v.count("zoomInVk") == 1);
    CHECK(v.count("profile") == 0);
    CHECK(v.count("onboarded") == 0);
    CHECK(v.count("uiTheme") == 0);
    CHECK(v.count("showAdvanced") == 0);
    CHECK(p.find("; a comment") != std::string::npos);   // comments survive
}

TEST_CASE("MakeLiveText: profile keys win, globals carry over, pointer set") {
    const std::string oldLive =
        "maxLevel=8.0\nmodel=render\nprofile=Default\nonboarded=1\nuiTheme=dark\nshowAdvanced=1\n";
    const std::string prof = "maxLevel=4.0\nmodel=transform\nzoomInVk=33\n";
    auto v = ReadIniValues(MakeLiveText(prof, oldLive, "Gaming"));
    CHECK(v["maxLevel"] == "4.0");
    CHECK(v["model"] == "transform");
    CHECK(v["zoomInVk"] == "33");
    CHECK(v["profile"] == "Gaming");
    CHECK(v["onboarded"] == "1");
    CHECK(v["uiTheme"] == "dark");
    CHECK(v["showAdvanced"] == "1");
}

TEST_CASE("MakeLiveText: empty profile means factory defaults (profile keys dropped)") {
    const std::string oldLive = "maxLevel=8.0\nzoomInVk=33\nonboarded=1\nprofile=Old\n";
    const std::string live = MakeLiveText("", oldLive, "Fresh");
    auto v = ReadIniValues(live);
    CHECK(v.count("maxLevel") == 0);          // gone -> ParseConfig default
    CHECK(v.count("zoomInVk") == 0);
    CHECK(v["onboarded"] == "1");             // global survives
    CHECK(v["profile"] == "Fresh");
    Config c = ParseConfig(live);
    CHECK(c.maxLevel == doctest::Approx(12.0));   // built-in default
    CHECK(c.zoomInVk == 0);
}

TEST_CASE("MakeLiveText strips global keys smuggled into a profile file") {
    // A hand-edited profile containing onboarded=0 must NOT reset onboarding on switch.
    const std::string prof = "onboarded=0\nmaxLevel=4.0\n";
    const std::string oldLive = "onboarded=1\nprofile=A\n";
    auto v = ReadIniValues(MakeLiveText(prof, oldLive, "B"));
    CHECK(v["onboarded"] == "1");
    CHECK(v["maxLevel"] == "4.0");
}

TEST_CASE("round trip: switch A -> B -> A preserves A's settings including keybinds") {
    const std::string liveA =
        "maxLevel=8.0\nzoomInVk=33\nzoomInButton=2\nprofile=A\nonboarded=1\n";
    const std::string profA = MakeProfileText(liveA);
    const std::string profB = "maxLevel=2.0\n";
    const std::string liveB = MakeLiveText(profB, liveA, "B");
    auto vB = ReadIniValues(liveB);
    CHECK(vB["maxLevel"] == "2.0");
    CHECK(vB.count("zoomInVk") == 0);         // B never set it -> default (per-profile keybinds)
    const std::string liveA2 = MakeLiveText(profA, liveB, "A");
    auto vA = ReadIniValues(liveA2);
    CHECK(vA["maxLevel"] == "8.0");
    CHECK(vA["zoomInVk"] == "33");
    CHECK(vA["zoomInButton"] == "2");
    CHECK(vA["onboarded"] == "1");
    CHECK(vA["profile"] == "A");
}

TEST_CASE("NextCopyName picks the first free suffix, case-insensitively") {
    CHECK(NextCopyName("Gaming", {"Gaming"}) == "Gaming copy");
    CHECK(NextCopyName("Gaming", {"Gaming", "Gaming copy"}) == "Gaming copy 2");
    CHECK(NextCopyName("Gaming", {"Gaming", "gaming copy", "Gaming copy 2"}) == "Gaming copy 3");
}

TEST_CASE("NextCopyName keeps the result inside the 40-char name cap") {
    const std::string base(40, 'x');   // itself at the cap
    const std::string c1 = NextCopyName(base, {base});
    CHECK(ProfileNameError(c1) == "");
    CHECK(c1.size() <= 40);
    const std::string c2 = NextCopyName(base, {base, c1});
    CHECK(ProfileNameError(c2) == "");
    CHECK(c2 != c1);
}

TEST_CASE("leading dots are rejected like trailing ones") {
    CHECK(ProfileNameError(".hidden") != "");
    CHECK(ProfileNameError("a.b") == "");   // interior dots stay fine
}

TEST_CASE("SameProfileName is ASCII case-insensitive equality") {
    CHECK(SameProfileName("Gaming", "gAMING"));
    CHECK_FALSE(SameProfileName("Gaming", "Gaming 2"));
}

TEST_CASE("ProfileTextError accepts real and factory-default profiles, rejects garbage") {
    CHECK(ProfileTextError("") == "");                            // factory defaults
    CHECK(ProfileTextError("; comment only\n") == "");            // factory defaults
    CHECK(ProfileTextError("maxLevel=8.0\n") == "");              // normal profile
    CHECK(ProfileTextError(std::string("bin\0ary", 7)) != "");    // NUL byte
    CHECK(ProfileTextError("this is not an ini\nat all\n") != ""); // lines but zero keys
    CHECK(ProfileTextError(std::string(300 * 1024, 'a')) != "");  // absurd size
}

TEST_CASE("SessionDiffers: identical texts do not differ") {
    CHECK_FALSE(SessionDiffers("model=a\nzoom=2\n", "model=a\nzoom=2\n"));
}
TEST_CASE("SessionDiffers: one changed key differs") {
    CHECK(SessionDiffers("model=a\nzoom=3\n", "model=a\nzoom=2\n"));
}
TEST_CASE("SessionDiffers: global keys are ignored") {
    CHECK_FALSE(SessionDiffers("model=a\nuiTheme=dark\nprofile=x\n", "model=a\nuiTheme=light\n"));
}
TEST_CASE("SessionDiffers: a key present on one side only differs") {
    CHECK(SessionDiffers("model=a\nzoom=2\n", "model=a\n"));
    CHECK(SessionDiffers("model=a\n", "model=a\nzoom=2\n"));
}
TEST_CASE("SessionDiffers: CRLF and trailing spaces do not differ") {
    CHECK_FALSE(SessionDiffers("model=a  \r\nzoom=2\r\n", "model=a\nzoom=2\n"));
}
TEST_CASE("UpdateProfileKey updates profile keys and refuses global keys") {
    CHECK(UpdateProfileKey("zoom=2\n", "zoom", "3") == UpdateIniText("zoom=2\n", "zoom", "3"));
    CHECK(UpdateProfileKey("zoom=2\n", "uiTheme", "dark") == "zoom=2\n");
}
TEST_CASE("uiPalette survives a profile switch and never lands in a profile file (#318)") {
    const std::string live = "maxLevel=8\nuiPalette=ember\nprofile=A\n";
    CHECK(MakeProfileText(live) == "maxLevel=8\n");
    const std::string sw = MakeLiveText("maxLevel=3\nuiPalette=hicon\n", live, "B");
    auto v = ReadIniValues(sw);
    CHECK(v["uiPalette"] == "ember");     // the live global wins; a smuggled one in the profile is dropped
    CHECK(v["maxLevel"] == "3");
    CHECK_FALSE(SessionDiffers("a=1\nuiPalette=ember\n", "a=1\nuiPalette=hicon\n"));
}
TEST_CASE("SessionDiffers: an explicit default equals a missing key (tray drag back to default)") {
    // panKeysOn is in the first-run template with its default; the tray writes the key, the profile never had it.
    const auto d = ReadIniValues(DefaultIniText());
    REQUIRE(d.count("panKeysOn") == 1);
    const std::string dv = d.at("panKeysOn");
    CHECK_FALSE(SessionDiffers("maxLevel=8\npanKeysOn=" + dv + "\n", "maxLevel=8\n"));
    CHECK_FALSE(SessionDiffers("maxLevel=8\n", "maxLevel=8\npanKeysOn=" + dv + "\n"));
    // A non-default value is still a change, in either direction.
    const std::string other = dv == "0" ? "1" : "0";
    CHECK(SessionDiffers("maxLevel=8\npanKeysOn=" + other + "\n", "maxLevel=8\n"));
    CHECK(SessionDiffers("maxLevel=8\n", "maxLevel=8\npanKeysOn=" + other + "\n"));
}
TEST_CASE("SessionDiffers: numeric spelling does not count as a change") {
    CHECK_FALSE(SessionDiffers("zoomInSpeed=1.0\n", "zoomInSpeed=1\n"));
    CHECK_FALSE(SessionDiffers("zoomInSpeed=1.50\n", "zoomInSpeed=1.5\n"));
    CHECK(SessionDiffers("zoomInSpeed=1.5\n", "zoomInSpeed=1.6\n"));
    CHECK(SessionDiffers("model=render\n", "model=hybrid\n"));        // text values still compare as text
}
TEST_CASE("SessionDiffers: keys outside the template stay missing-vs-present") {
    CHECK(SessionDiffers("someFutureKey=0\n", ""));
    CHECK(SessionDiffers("", "someFutureKey=0\n"));
}
TEST_CASE("ParseIniTmpName recognises WriteTextFileAtomic's leftover temp names") {
    unsigned long pid = 0;
    CHECK(ParseIniTmpName(L"magnifier.ini.1234.tmp", pid)); CHECK(pid == 1234);
    CHECK(ParseIniTmpName(L"Game night.ini.77.tmp", pid));  CHECK(pid == 77);
    CHECK(ParseIniTmpName(L"MAGNIFIER.INI.5.TMP", pid));    CHECK(pid == 5);
    CHECK_FALSE(ParseIniTmpName(L"magnifier.ini.tmp", pid));        // no pid
    CHECK_FALSE(ParseIniTmpName(L"magnifier.ini.12a.tmp", pid));    // not all digits
    CHECK_FALSE(ParseIniTmpName(L"magnifier.ini.1234", pid));       // not a temp
    CHECK_FALSE(ParseIniTmpName(L"notes.txt.1234.tmp", pid));       // not an ini temp: never touch other files
    CHECK_FALSE(ParseIniTmpName(L".ini.1234.tmp", pid));            // empty stem
    CHECK_FALSE(ParseIniTmpName(L"x.ini.99999999999.tmp", pid));    // pid out of range
}
TEST_CASE("uiHighResNoticeOff is global: stripped from profiles, carried into the live text (#441)") {
    CHECK(IsGlobalProfileKey("uiHighResNoticeOff"));
    CHECK(MakeProfileText("a=1\nuiHighResNoticeOff=1\n") == "a=1\n");
    const std::string live = MakeLiveText("a=2\n", "a=1\nuiHighResNoticeOff=1\n", "P");
    CHECK(live.find("uiHighResNoticeOff=1") != std::string::npos);
    CHECK(live.find("a=2") != std::string::npos);
}
TEST_CASE("StripUiOnlyKeys strips every global key except profile (tray edits must not reload the core)") {
    const char* globals[] = { "onboarded", "uiTheme", "uiPalette", "showAdvanced", "trayPerf", "traySliders",
                              "traySliderOrder", "trayToggles", "trayToggleOrder", "trayPinned", "uiHighResNoticeOff" };
    for (const char* k : globals) {
        REQUIRE(IsGlobalProfileKey(k));
        const std::string line = std::string(k) + "=1\n";
        CHECK(StripUiOnlyKeys("maxLevel=8\n" + line) == "maxLevel=8\n");
        CHECK(StripUiOnlyKeys("maxLevel=8\n" + std::string(k) + "=2\n") == StripUiOnlyKeys("maxLevel=8\n" + line));
        // The carry-over list in MakeLiveText keeps the live value across a profile switch; profiles never store it.
        CHECK(ReadIniValues(MakeLiveText("", std::string(k) + "=0\n", "X"))[k] == "0");
        CHECK(MakeProfileText("maxLevel=8\n" + line).find(std::string(k) + "=") == std::string::npos);
    }
    // profile stays IN the fingerprint: a profile switch must still reload the core.
    CHECK(StripUiOnlyKeys("profile=A\n") != StripUiOnlyKeys("profile=B\n"));
    // Prefix siblings are different keys.
    CHECK(StripUiOnlyKeys("traySlidersX=1\n") == "traySlidersX=1\n");
}

TEST_CASE("profile names: a device name is reserved with any extension, and the cap counts characters (review 2026-10-09 #73, #90)") {
    CHECK(ProfileNameError("con.x") != "");
    CHECK(ProfileNameError("NUL.txt") != "");
    CHECK(ProfileNameError("com1 .ini") != "");
    CHECK(ProfileNameError("Lpt9.a.b") != "");
    CHECK(ProfileNameError("console") == "");        // only the exact device stem is reserved
    CHECK(ProfileNameError("con x") == "");
    // 40 two-byte characters (80 bytes) fit; 41 do not.
    std::string accents; for (int i = 0; i < 40; ++i) accents += "\xC3\xA6";
    CHECK(ProfileNameError(accents) == "");
    CHECK(ProfileNameError(accents + "\xC3\xA6") != "");
}

TEST_CASE("SessionDiffers: UI keys outside the first-run template read as their built-in default (review #70, #76)") {
    CHECK_FALSE(SessionDiffers("zoomEaseOutMs=45\n", "maxLevel=12\n"));
    CHECK(SessionDiffers("zoomEaseOutMs=100\n", "maxLevel=12\n"));
    CHECK_FALSE(SessionDiffers("txSamplingMode=0\n", ""));
    CHECK(SessionDiffers("txSamplingMode=1\n", ""));
    CHECK_FALSE(SessionDiffers("lockApps=\n", ""));
    CHECK(SessionDiffers("lockApps=game.exe\n", ""));
    // The core's missing-key engine is hybrid, the template's too: no special case for `model`.
    CHECK_FALSE(SessionDiffers("model=hybrid\n", ""));
    CHECK(SessionDiffers("model=render\n", ""));
}
