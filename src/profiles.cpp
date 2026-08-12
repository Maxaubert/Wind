#include "profiles.h"
#include "config_ui/ini_edit.h"
#include <sstream>
namespace wind {
static std::string lower(const std::string& s) {
    std::string o = s;
    for (auto& c : o) if (c >= 'A' && c <= 'Z') c += 32;
    return o;
}
static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}
bool IsGlobalProfileKey(const std::string& key) {
    return key == "profile" || key == "onboarded" || key == "uiTheme" || key == "showAdvanced";
}
std::string ProfileNameError(const std::string& name) {
    if (name.empty()) return "Name cannot be empty";
    if (name.size() > 40) return "Name is too long (max 40 characters)";
    for (unsigned char c : name) {
        if (c < 0x20) return "Name contains a control character";
        if (std::string("\\/:*?\"<>|").find((char)c) != std::string::npos)
            return "Name cannot contain \\ / : * ? \" < > |";
    }
    if (name.front() == ' ' || name.back() == ' ') return "Name cannot start or end with a space";
    if (name.front() == '.' || name.back() == '.') return "Name cannot start or end with a dot";
    static const char* reserved[] = {"con","prn","aux","nul",
        "com1","com2","com3","com4","com5","com6","com7","com8","com9",
        "lpt1","lpt2","lpt3","lpt4","lpt5","lpt6","lpt7","lpt8","lpt9"};
    const std::string l = lower(name);
    for (const char* r : reserved) if (l == r) return "That name is reserved by Windows";
    return "";
}
bool ProfileNameTaken(const std::string& name, const std::vector<std::string>& names) {
    const std::string l = lower(name);
    for (const auto& n : names) if (lower(n) == l) return true;
    return false;
}
// Shared line filter: drop every line whose key satisfies IsGlobalProfileKey; keep the rest verbatim.
static std::string StripGlobalKeyLines(const std::string& text) {
    std::istringstream in(text);
    std::string line, out;
    while (std::getline(in, line)) {
        std::string t = trim(line);
        bool drop = false;
        if (!t.empty() && t[0] != ';' && t[0] != '#') {
            size_t eq = t.find('=');
            if (eq != std::string::npos && IsGlobalProfileKey(trim(t.substr(0, eq)))) drop = true;
        }
        if (!drop) out += line + "\n";
    }
    return out;
}
std::string MakeProfileText(const std::string& liveText) { return StripGlobalKeyLines(liveText); }
std::string MakeLiveText(const std::string& profileText, const std::string& oldLiveText,
                         const std::string& name) {
    std::string out = StripGlobalKeyLines(profileText);
    auto oldVals = ReadIniValues(oldLiveText);
    for (const char* k : {"onboarded", "uiTheme", "showAdvanced"}) {
        auto it = oldVals.find(k);
        if (it != oldVals.end()) out = UpdateIniText(out, k, it->second);
    }
    return UpdateIniText(out, "profile", name);
}
std::string NextCopyName(const std::string& base, const std::vector<std::string>& names) {
    // Truncate the base so "<base><suffix>" always fits the 40-char cap (the suffix grows with N).
    auto fit = [&](const std::string& suffix) {
        std::string b = base;
        if (b.size() + suffix.size() > 40) b = trim(b.substr(0, 40 - suffix.size()));
        return b + suffix;
    };
    std::string cand = fit(" copy");
    for (int i = 2; ProfileNameTaken(cand, names); ++i)
        cand = fit(" copy " + std::to_string(i));
    return cand;
}
bool SameProfileName(const std::string& a, const std::string& b) { return lower(a) == lower(b); }
std::string ProfileTextError(const std::string& text) {
    if (text.size() > 256 * 1024) return "Profile file is unreasonably large";
    if (text.find('\0') != std::string::npos) return "Profile file is not a text file";
    if (!ReadIniValues(text).empty()) return "";
    // Zero keys parsed: fine only if every line is blank or a comment (factory defaults).
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        std::string t = trim(line);
        if (!t.empty() && t[0] != ';' && t[0] != '#') return "Profile file is not a Wind profile";
    }
    return "";
}
}
