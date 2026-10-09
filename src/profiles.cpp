#include "profiles.h"
#include "config_ui/ini_edit.h"
#include "config.h"   // DefaultIniText: the built-in default of every templated key
#include <sstream>
#include <cstdlib>
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
    return key == "profile" || key == "onboarded" || key == "uiTheme" || key == "uiPalette" || key == "showAdvanced" ||
           key == "trayPerf" || key == "traySliders" || key == "traySliderOrder" ||
           key == "trayToggles" || key == "trayToggleOrder";
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
    for (const char* k : {"onboarded", "uiTheme", "uiPalette", "showAdvanced", "trayPerf", "traySliders",
                           "traySliderOrder", "trayToggles", "trayToggleOrder"}) {
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
// Two ini values mean the same setting: identical text, or both numbers with the same value
// ("1.0" vs "1").
static bool SameIniValue(const std::string& x, const std::string& y) {
    if (x == y) return true;
    if (x.empty() || y.empty()) return false;
    char* ex = nullptr; char* ey = nullptr;
    const double dx = std::strtod(x.c_str(), &ex);
    const double dy = std::strtod(y.c_str(), &ey);
    return *ex == '\0' && *ey == '\0' && dx == dy;
}
bool SessionDiffers(const std::string& liveText, const std::string& profileText) {
    auto a = ReadIniValues(liveText);
    auto b = ReadIniValues(profileText);
    for (auto it = a.begin(); it != a.end();) it = IsGlobalProfileKey(it->first) ? a.erase(it) : std::next(it);
    for (auto it = b.begin(); it != b.end();) it = IsGlobalProfileKey(it->first) ? b.erase(it) : std::next(it);
    // A key one side lacks reads as the built-in default (the first-run template carries each
    // templated key's default), so a tray drag that wrote an explicit default back (Warmth 0 -> 40
    // -> 0) is not "unsaved". Keys outside the template stay missing-vs-present. "model" is left
    // out: the core's missing-key engine and the template's explicit one are different questions
    // (see createProfile in config_ui/main.cpp).
    static const std::map<std::string, std::string> defaults = ReadIniValues(DefaultIniText());
    auto valueOf = [&](const std::map<std::string, std::string>& m, const std::string& k, std::string& out) {
        auto it = m.find(k);
        if (it != m.end()) { out = it->second; return true; }
        if (k == "model") return false;
        auto d = defaults.find(k);
        if (d == defaults.end()) return false;
        out = d->second;
        return true;
    };
    std::vector<std::string> keys;
    for (const auto& kv : a) keys.push_back(kv.first);
    for (const auto& kv : b) keys.push_back(kv.first);
    for (const auto& k : keys) {
        std::string va, vb;
        const bool ha = valueOf(a, k, va), hb = valueOf(b, k, vb);
        if (ha != hb) return true;
        if (ha && !SameIniValue(va, vb)) return true;
    }
    return false;
}
bool ParseIniTmpName(const std::wstring& fileName, unsigned long& pid) {
    auto low = [](wchar_t c) { return (c >= L'A' && c <= L'Z') ? (wchar_t)(c + 32) : c; };
    const std::wstring tmp = L".tmp", ini = L".ini";
    if (fileName.size() <= tmp.size() + ini.size() + 2) return false;   // stem + ".ini" + "." + digit + ".tmp"
    const size_t end = fileName.size() - tmp.size();
    for (size_t i = 0; i < tmp.size(); ++i) if (low(fileName[end + i]) != tmp[i]) return false;
    size_t d = end;
    while (d > 0 && fileName[d - 1] >= L'0' && fileName[d - 1] <= L'9') --d;
    if (d == end || d == 0 || fileName[d - 1] != L'.' || end - d > 10) return false;
    const size_t stemEnd = d - 1;   // the "." before the pid
    if (stemEnd < ini.size() + 1) return false;
    for (size_t i = 0; i < ini.size(); ++i) if (low(fileName[stemEnd - ini.size() + i]) != ini[i]) return false;
    unsigned long long v = 0;
    for (size_t i = d; i < end; ++i) v = v * 10 + (unsigned)(fileName[i] - L'0');
    if (v > 0xFFFFFFFFULL) return false;
    pid = (unsigned long)v;
    return true;
}
std::string UpdateProfileKey(const std::string& profileText, const std::string& key,
                             const std::string& value) {
    if (IsGlobalProfileKey(key)) return profileText;
    return UpdateIniText(profileText, key, value);
}
}
