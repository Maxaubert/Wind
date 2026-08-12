#include "doctest.h"
#include "../src/profiles.h"
#include "../src/config_ui/ini_edit.h"
#include "../src/config.h"
using namespace wind;

TEST_CASE("global keys are exactly profile/onboarded/uiTheme/showAdvanced") {
    CHECK(IsGlobalProfileKey("profile"));
    CHECK(IsGlobalProfileKey("onboarded"));
    CHECK(IsGlobalProfileKey("uiTheme"));
    CHECK(IsGlobalProfileKey("showAdvanced"));
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
    const std::string prof = "maxLevel=4.0\nmodel=magnify\nzoomInVk=33\n";
    auto v = ReadIniValues(MakeLiveText(prof, oldLive, "Gaming"));
    CHECK(v["maxLevel"] == "4.0");
    CHECK(v["model"] == "magnify");
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
