#include "doctest.h"
#include "../src/config_ui/ini_edit.h"
using namespace wind;

TEST_CASE("ReadIniValues parses key=value, skipping comments and blanks") {
    auto m = ReadIniValues("; c\nmaxLevel=8.0\n\nzoomInSpeed = 1.2\n# x\n");
    CHECK(m["maxLevel"] == "8.0");
    CHECK(m["zoomInSpeed"] == "1.2");
    CHECK(m.count("c") == 0);
}
TEST_CASE("UpdateIniText replaces an existing key in place, preserving the rest") {
    std::string t = "; speed knob\nzoomInSpeed=1.0\nmaxLevel=8.0\n";
    std::string r = UpdateIniText(t, "zoomInSpeed", "2.0");
    auto m = ReadIniValues(r);
    CHECK(m["zoomInSpeed"] == "2.0");
    CHECK(m["maxLevel"] == "8.0");
    CHECK(r.find("; speed knob") != std::string::npos);
}
TEST_CASE("UpdateIniText appends a missing key") {
    std::string r = UpdateIniText("maxLevel=8.0\n", "smoothZoom", "1");
    auto m = ReadIniValues(r);
    CHECK(m["smoothZoom"] == "1");
    CHECK(m["maxLevel"] == "8.0");
}
TEST_CASE("UpdateIniText leaves unknown keys and comment-only lines intact") {
    std::string t = "; header\nfoo=bar\n; mid\nzoomInSpeed=1.0\n";
    std::string r = UpdateIniText(t, "zoomInSpeed", "3.0");
    CHECK(r.find("foo=bar") != std::string::npos);
    CHECK(r.find("; mid") != std::string::npos);
    CHECK(ReadIniValues(r)["zoomInSpeed"] == "3.0");
}
TEST_CASE("read-modify-write round trip is stable") {
    std::string r = UpdateIniText(UpdateIniText("a=1\nb=2\n", "a", "10"), "c", "3");
    auto m = ReadIniValues(r);
    CHECK(m["a"] == "10"); CHECK(m["b"] == "2"); CHECK(m["c"] == "3");
}

TEST_CASE("UpdateIniText collapses duplicate keys so last-wins readers see the write") {
    // A hand-edited ini (openIni) can gain a duplicate key. Readers are last-wins, so replacing
    // only the first line made the write invisible and every later update a permanent no-op.
    const std::string dup = "maxLevel=12.0\nzoomInVk=33\nmaxLevel=20\n";
    const std::string out = wind::UpdateIniText(dup, "maxLevel", "30");
    CHECK(wind::ReadIniValues(out)["maxLevel"] == "30");
    CHECK(out.find("maxLevel=20") == std::string::npos);   // the duplicate is gone
    CHECK(out.find("zoomInVk=33") != std::string::npos);   // other keys untouched
}
