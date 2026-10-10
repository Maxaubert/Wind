#include "doctest.h"
#include "../src/keybind_rules.h"
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
using namespace wind;

// Fixture spelling of each verdict; only this test needs the names.
static inline const char* BindVerdictName(BindVerdict v) {
    switch (v) {
        case BindVerdict::Ok: return "ok";
        case BindVerdict::NeverBindable: return "never";
        case BindVerdict::ModifierAsKey: return "modifier";
        case BindVerdict::NotAlone: return "notalone";
        case BindVerdict::ShiftTypes: return "shifttypes";
        case BindVerdict::AltGrTypes: return "altgr";
        case BindVerdict::SystemReserved: return "system";
        case BindVerdict::WindowsReserved: return "windows";
        case BindVerdict::NeedsModifier: return "needsmod";
        case BindVerdict::CtrlAlone: return "ctrlalone";
        case BindVerdict::ShiftAlone: return "shiftalone";
    }
    return "?";
}

// Runs every case in the shared list (also read by the UI mirror's tests).
TEST_CASE("keybind safety rules match the shared case list (#285)") {
    // The cwd varies (repo root from build.bat, a build dir elsewhere), so also try the folder
    // this file sits in, then walk up from the cwd.
    std::string here = __FILE__;
    const size_t cut = here.find_last_of("/\\");
    std::vector<std::string> tries;
    if (cut != std::string::npos) tries.push_back(here.substr(0, cut) + "/fixtures/keybind_cases.txt");
    for (const char* up : { "", "../", "../../", "../../../" })
        tries.push_back(std::string(up) + "tests/fixtures/keybind_cases.txt");
    std::ifstream f;
    for (const std::string& p : tries) { f.open(p); if (f.good()) break; f.clear(); }
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
