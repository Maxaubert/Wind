#pragma once
// The tray tools' PURE logic (issue #315, no <windows.h>, unit-tested): the per-kind engine
// dropdown's keys and captions, the listen-and-chime state machine for the Mouse lock / Pass keys
// chips, the exe-list edits it applies, and the pulse curve. tools.cpp and flyout_window.cpp wire
// these to Win32. Spec: docs/superpowers/specs/2026-10-02-tray-tools-design.md.
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>
#include "../engine_pick.h"

namespace wind { namespace Flyout {

// ---------------------------------------------------------------- engine dropdown

// WindowCategory values as the core publishes them (0 game, 1 acrylic, 2 desktop, 3 other).
// Anything else (-1 = nothing in front yet) falls back to Other.
inline int NormCategory(int c) { return (c >= 0 && c <= 3) ? c : 3; }

inline const char* EngineKeyFor(int category) {
    switch (NormCategory(category)) {
        case 0: return "engineGame";
        case 1: return "engineAcrylic";
        case 2: return "engineDesktop";
        default: return "engineOther";
    }
}
inline const wchar_t* EngineCaptionFor(int category) {
    switch (NormCategory(category)) {
        case 0: return L"Engine for games";
        case 1: return L"Engine for blurred windows";
        case 2: return L"Engine for the desktop";
        default: return L"Engine for other windows";
    }
}
// The three values of a per-window engine row, in list order (same as Settings: Auto, Transform, Render).
inline const char* EnginePrefValue(int i) { return i == 1 ? "transform" : (i == 2 ? "render" : "auto"); }
inline const wchar_t* EnginePrefLabel(int i) { return i == 1 ? L"Transform" : (i == 2 ? L"Render" : L"Auto"); }
inline int EnginePrefIndex(const std::string& v) { return (int)ParseEnginePref(v); }

// The per-window rows only act when the main engine (`model`) is Auto (= "hybrid", also the default
// for a missing or unknown value). Otherwise the dropdown is disabled and names the main engine.
inline bool MainEngineIsAuto(const std::string& model) {
    return !(model == "render" || model == "transform" || model == "magnify");
}
inline std::wstring MainEngineCaption(const std::string& model) {
    if (model == "render") return L"Main engine: Render";
    if (model == "transform") return L"Main engine: Transform";
    return L"Main engine: System";
}

// ---------------------------------------------------------------- exe lists (lockApps, noSwallowApps)

enum class FixKind { Lock, Pass };
inline const char* FixKey(FixKind k) { return k == FixKind::Lock ? "lockApps" : "noSwallowApps"; }

inline std::string LowerAscii(std::string s) {
    for (auto& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}
inline std::string TrimWs(const std::string& s) {
    const size_t b = s.find_first_not_of(" \t\r\n"), e = s.find_last_not_of(" \t\r\n");
    return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
}
inline std::vector<std::string> SplitExeList(const std::string& list) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos <= list.size()) {
        size_t c = list.find(',', pos);
        if (c == std::string::npos) c = list.size();
        const std::string t = TrimWs(list.substr(pos, c - pos));
        if (!t.empty()) out.push_back(t);
        pos = c + 1;
    }
    return out;
}
inline std::string JoinExeList(const std::vector<std::string>& v) {
    std::string s;
    for (const auto& t : v) { if (!s.empty()) s += ","; s += t; }
    return s;
}
inline bool ExeListHas(const std::string& list, const std::string& exe) {
    const std::string e = LowerAscii(TrimWs(exe));
    if (e.empty()) return false;
    for (const auto& t : SplitExeList(list)) if (LowerAscii(t) == e) return true;
    return false;
}
// Adds `exe` unless already there (case-insensitive): no duplicates. Other entries are kept as written.
inline std::string ExeListAdd(const std::string& list, const std::string& exe) {
    std::vector<std::string> v = SplitExeList(list);
    const std::string e = TrimWs(exe);
    if (e.empty() || ExeListHas(list, e)) return JoinExeList(v);
    v.push_back(e);
    return JoinExeList(v);
}
// Removes every entry equal to `exe` (case-insensitive).
inline std::string ExeListRemove(const std::string& list, const std::string& exe) {
    const std::string e = LowerAscii(TrimWs(exe));
    std::vector<std::string> v;
    for (const auto& t : SplitExeList(list)) if (LowerAscii(t) != e) v.push_back(t);
    return JoinExeList(v);
}

// ---------------------------------------------------------------- listen state machine

inline constexpr uint64_t kListenTimeoutMs = 20000;
inline constexpr uint64_t kListenPulseMs = 1200;      // one soft pulse

struct ListenState {
    bool active = false;
    FixKind kind = FixKind::Lock;
    uint32_t baseActivations = 0;     // app activations when listening began
    uint64_t startMs = 0;
};

enum class ListenEvent { Pending, Target, TimedOut };

inline ListenState ListenStart(FixKind kind, uint32_t activations, uint64_t nowMs) {
    ListenState s;
    s.active = true; s.kind = kind; s.baseActivations = activations; s.startMs = nowMs;
    return s;
}

// What a click on a listen chip does: a click on the chip that is listening cancels (silently);
// a click on the other chip (or none) starts that one, replacing any listen in progress.
inline ListenState ListenClick(const ListenState& cur, FixKind clicked, uint32_t activations, uint64_t nowMs) {
    if (cur.active && cur.kind == clicked) return ListenState{};
    return ListenStart(clicked, activations, nowMs);
}

// One poll. The target is the next real app activation after listening began (the core publishes
// only real apps: never the shell, the taskbar, the flyout or Wind's own windows), told by a grown
// activation count and a usable (non-empty) exe name; an activation whose name is unusable waits
// for the next one. A clock that went backwards never fires the timeout.
inline ListenEvent ListenPoll(const ListenState& s, uint32_t activations, const std::wstring& exe, uint64_t nowMs) {
    if (!s.active) return ListenEvent::Pending;
    if (activations > s.baseActivations && !exe.empty()) return ListenEvent::Target;
    if (nowMs >= s.startMs && nowMs - s.startMs >= kListenTimeoutMs) return ListenEvent::TimedOut;
    return ListenEvent::Pending;
}

// Slow soft pulse 0..1 (a cosine), `kListenPulseMs` per cycle, starting at 0 so it eases in.
inline float ListenPulse(uint64_t elapsedMs) {
    const double ph = (double)(elapsedMs % kListenPulseMs) / (double)kListenPulseMs;
    return (float)(0.5 - 0.5 * std::cos(ph * 6.283185307179586));
}

}}  // namespace wind::Flyout
