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

// Issue #156: suspending the LL keyboard hook trades key-swallowing for a smooth mouse stream in
// the target app. It must be OPT-IN, so an empty/absent config has to leave both off - that is what
// keeps the shipped behaviour (swallow everywhere) unchanged for anyone who never configures it.
TEST_CASE("keyboard-hook suspension is off by default") {
    CHECK(ParseConfig("").noSwallowApps.empty());
}

TEST_CASE("noSwallowApps parses") {
    Config c = ParseConfig("noSwallowApps=RDR2.exe,eldenring.exe\n");
    CHECK(c.noSwallowApps == "RDR2.exe,eldenring.exe");
}

// "key=" with nothing after it must CLEAR an exe list, not fall back to the shipped default. That
// is exactly what the config UI writes when the last entry is removed, and the generic parser skips
// empty values (stoi("") throws on the numeric settings), so the lists are handled ahead of that
// guard. Without this, clearing a list wrote the ini correctly and the core silently kept the old
// value - and a list with defaults could never be emptied at all.
TEST_CASE("an empty value clears an exe list instead of keeping the default") {
    CHECK(ParseConfig("transformExclude=\n").transformExclude.empty());
    CHECK(ParseConfig("noSwallowApps=RDR2.exe\nnoSwallowApps=\n").noSwallowApps.empty());
    CHECK_FALSE(ParseConfig("").transformExclude.empty());   // absent still means the default
}

// The list is matched with the same helper the transform exclusion uses, so it inherits
// case-insensitive exact-name matching. Guard that here: a substring match would suspend the hook
// for unrelated apps (e.g. "rdr2.exe" must not be matched by "dr2.exe").
TEST_CASE("noSwallowApps matching is case-insensitive and exact") {
    const std::string list = "RDR2.exe,eldenring.exe";
    CHECK(IsExeInList("rdr2.exe", list));
    CHECK(IsExeInList("RDR2.EXE", list));
    CHECK(IsExeInList("eldenring.exe", list));
    CHECK_FALSE(IsExeInList("dr2.exe", list));
    CHECK_FALSE(IsExeInList("notepad.exe", list));
    CHECK_FALSE(IsExeInList("rdr2.exe", ""));
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

TEST_CASE("transformExclude keeps fullscreen browsers on the render engine (Auto mode)") {
    // Default list ships the common browsers: fullscreen video looks like a game to the
    // foreground test but wants render (constant-size cursor, desktop-style behaviour).
    Config d = ParseConfig("");
    CHECK(IsExeInList("zen.exe", d.transformExclude));
    CHECK(IsExeInList("CHROME.EXE", d.transformExclude));       // case-insensitive
    CHECK(IsExeInList("msedge.exe", d.transformExclude));
    CHECK_FALSE(IsExeInList("foundation.exe", d.transformExclude));   // games still get transform
    // User-supplied lists: whitespace tolerated, exact name match only (no substrings).
    Config c = ParseConfig("transformExclude=vlc.exe, mpv.exe\n");
    CHECK(IsExeInList("mpv.exe", c.transformExclude));
    CHECK(IsExeInList("vlc.exe", c.transformExclude));
    CHECK_FALSE(IsExeInList("lc.exe", c.transformExclude));      // not a substring match
    CHECK_FALSE(IsExeInList("chrome.exe", c.transformExclude));  // replaced, not merged
    // An empty list excludes nothing.
    CHECK_FALSE(IsExeInList("zen.exe", ""));
}

TEST_CASE("maxLevel is one shared setting - no per-model clamp (issue #148 root-caused)") {
    // The old transform/hybrid <=12 cap guarded what turned out to be the NVIDIA 16-bit MPO
    // overflow; the mapper's pan wall handles that at any level, so all models share maxLevel.
    CHECK(ParseConfig("model=transform\nmaxLevel=20\n").maxLevel == doctest::Approx(20.0));
    CHECK(ParseConfig("model=hybrid\nmaxLevel=16\n").maxLevel == doctest::Approx(16.0));
    CHECK(ParseConfig("model=transform\nmaxLevel=8\n").maxLevel == doctest::Approx(8.0));
    CHECK(ParseConfig("model=render\nmaxLevel=20\n").maxLevel == doctest::Approx(20.0));
}
