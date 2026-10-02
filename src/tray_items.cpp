#include "tray_items.h"
#include <algorithm>
namespace wind {
const std::vector<std::string>& EligibleSliders() {
    static const std::vector<std::string> k = {"colorWarmPct", "colorDimPct", "maxLevel", "zoomInSpeed",
                                               "zoomOutSpeed", "panSpeed", "cursorSmoothing", "zoomEaseOutMs"};
    return k;
}
const std::vector<std::string>& EligibleToggles() {
    static const std::vector<std::string> k = {"trackCaret", "trackFocus", "keepEdges", "engine", "fixLock", "fixPass", "pause"};
    return k;
}

static std::string Trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n"), e = s.find_last_not_of(" \t\r\n");
    return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
}
static std::vector<std::string> SplitList(const std::string& s) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos <= s.size()) {
        size_t c = s.find(',', pos);
        if (c == std::string::npos) c = s.size();
        std::string t = Trim(s.substr(pos, c - pos));
        if (!t.empty()) out.push_back(t);
        pos = c + 1;
    }
    return out;
}
static bool Has(const std::vector<std::string>& v, const std::string& k) {
    return std::find(v.begin(), v.end(), k) != v.end();
}
static const std::string* Find(const IniValues& v, const char* key) {
    auto it = v.find(key);
    return it == v.end() ? nullptr : &it->second;
}

// Order = the order key's known items, then the enabled list's known items not yet seen, then every
// remaining eligible item. Enabled = the enabled list (defaults when its key is absent).
static std::vector<TrayItem> ParseList(const IniValues& v, const char* enabledKey, const char* orderKey,
                                       const std::vector<std::string>& eligible,
                                       const std::vector<std::string>& defaultOn, int cap) {
    const std::string* en = Find(v, enabledKey);
    const std::string* od = Find(v, orderKey);
    std::vector<std::string> enabled = en ? SplitList(*en) : defaultOn;
    std::vector<std::string> order;
    auto add = [&](const std::string& k) {
        if (Has(eligible, k) && !Has(order, k)) order.push_back(k);
    };
    if (od) for (const auto& k : SplitList(*od)) add(k);
    for (const auto& k : enabled) add(k);
    for (const auto& k : eligible) add(k);
    std::vector<TrayItem> out;
    int on = 0;
    for (const auto& k : order) {
        TrayItem it;
        it.key = k;
        if (Has(enabled, k) && (cap <= 0 || on < cap)) { it.on = true; ++on; }
        out.push_back(it);
    }
    return out;
}
static std::string Join(const std::vector<TrayItem>& items, bool onlyEnabled) {
    std::string s;
    for (const auto& it : items) {
        if (onlyEnabled && !it.on) continue;
        if (!s.empty()) s += ",";
        s += it.key;
    }
    return s;
}

TrayLayout ParseTrayLayout(const IniValues& v) {
    TrayLayout l;
    const std::string* p = Find(v, "trayPerf");
    l.perf = p && Trim(*p) == "1";
    l.sliders = ParseList(v, "traySliders", "traySliderOrder", EligibleSliders(),
                          {"colorWarmPct", "colorDimPct"}, kMaxTraySliders);
    l.toggles = ParseList(v, "trayToggles", "trayToggleOrder", EligibleToggles(), {}, 0);
    return l;
}
void WriteTrayLayout(const TrayLayout& l, IniValues& v) {
    v["trayPerf"] = l.perf ? "1" : "0";
    v["traySliders"] = Join(l.sliders, true);
    v["traySliderOrder"] = Join(l.sliders, false);
    v["trayToggles"] = Join(l.toggles, true);
    v["trayToggleOrder"] = Join(l.toggles, false);
}
}
