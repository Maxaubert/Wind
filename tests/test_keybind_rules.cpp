#include "doctest.h"
#include "../src/keybind_rules.h"
#include <fstream>
#include <sstream>
#include <string>
using namespace wind;

// Runs every case in the shared list (also read by the UI mirror's tests).
TEST_CASE("keybind safety rules match the shared case list (#285)") {
    std::ifstream f("tests/fixtures/keybind_cases.txt");
    REQUIRE(f.good());
    std::string line;
    int cases = 0;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == 0x0D) line.pop_back();   // CRLF checkouts
        if (line.empty() || line[0] == 0x23) continue;
        std::istringstream in(line);
        std::string kind, want;
        in >> kind;
        const char* got = "";
        if (kind == "key") {
            std::string vkHex; int mods = 0;
            in >> vkHex >> mods >> want;
            got = BindVerdictName(CheckKeyBind((int)std::stoul(vkHex, nullptr, 16), mods));
        } else if (kind == "wheel") {
            int mods = 0; in >> mods >> want;
            got = BindVerdictName(CheckWheelBind(mods));
        } else if (kind == "click") {
            int btn = 0, mods = 0; in >> btn >> mods >> want;
            got = BindVerdictName(CheckClickBind(btn, mods));
        } else {
            FAIL("unknown case kind: " << line);
        }
        INFO(line);
        CHECK(std::string(got) == want);
        ++cases;
    }
    CHECK(cases > 90);
}
TEST_CASE("an unbound slot is always fine") {
    CHECK(CheckKeyBind(0, 0) == BindVerdict::Ok);
    CHECK(CheckKeyBind(0, 15) == BindVerdict::Ok);
}
