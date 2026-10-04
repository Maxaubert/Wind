// Pure tray quick-control list model (no <windows.h>): which sliders and toggles the tray flyout
// shows and in what order, parsed from / written to the global (non-profile) ini keys
// trayPerf, traySliders, traySliderOrder, trayToggles, trayToggleOrder. Shared by WindTray and the
// config host. Spec: docs/specs/2026-10-01-tray-flyout-design.md (issue #313).
#pragma once
#include <map>
#include <string>
#include <vector>
namespace wind {
using IniValues = std::map<std::string, std::string>;

struct TrayItem {
    std::string key;
    bool on = false;
};
struct TrayLayout {
    bool perf = false;
    std::vector<TrayItem> sliders;  // full order, enabled flag per item
    std::vector<TrayItem> toggles;
};

// At most this many sliders are enabled at once; extra enabled ones read as off (list order).
constexpr int kMaxTraySliders = 4;

// Eligible items, in their default order. Sliders are ini keys; toggles are ini keys except
// "keepEdges", the combined mouseAlign + trackAlign item. "engine" is not a toggle: it is the wide
// main-engine dropdown (the `model` key, two chip slots). All default off. Keys a user's ini still
// lists from an earlier 0.18 dev build (fixLock, fixPass, pause) are unknown and silently dropped.
// Spec: 2026-10-02-tray-tools-design.md (#315).
const std::vector<std::string>& EligibleSliders();
const std::vector<std::string>& EligibleToggles();

// Missing keys give the defaults (Performance off; Warmth and Brightness on; toggles off). Unknown
// keys are dropped, eligible items missing from the order are appended off, and enabled sliders
// beyond kMaxTraySliders are read as off.
TrayLayout ParseTrayLayout(const IniValues& v);
// Sets the five keys; other entries are untouched.
void WriteTrayLayout(const TrayLayout& layout, IniValues& v);
}
