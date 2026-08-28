#pragma once
// What the tray shows about the running magnifier. PURE (no <windows.h>): the label decisions are
// where the bugs live, so they are unit-tested rather than eyeballed in a menu.
//
// The tick loop publishes a snapshot; the tray reads it when the menu opens. There is no live
// update while the menu is open and that is deliberate (docs/superpowers/specs/2026-08-28-tray-menu-design.md):
// the values are current at the moment you open it, which is all a menu needs to be truthful.
#include <atomic>

namespace wind {

// Which engine is actually running this session - NOT what the ini asked for. "Advanced" is the
// hybrid model: it is the mode that picks per window type, which is what the name should say.
enum class TrayEngine { Advanced, Transform, Render, System };

struct TrayStatus {
    double      level   = 1.0;                    // 1.0 = not zoomed
    TrayEngine  engine  = TrayEngine::Advanced;
    bool        panning = false;
};

// One writer (the tick loop), one reader (the tray). Three plain scalars behind a seqlock-free
// relaxed publish: the worst a torn read can do is pair last tick's level with this tick's engine,
// which is invisible in a menu that opens in 30ms.
inline std::atomic<double>& TrayLevelSlot()   { static std::atomic<double> v{1.0}; return v; }
inline std::atomic<int>&    TrayEngineSlot()  { static std::atomic<int> v{0}; return v; }
inline std::atomic<bool>&   TrayPanningSlot() { static std::atomic<bool> v{false}; return v; }

inline void PublishTrayStatus(const TrayStatus& s) {
    TrayLevelSlot().store(s.level, std::memory_order_relaxed);
    TrayEngineSlot().store((int)s.engine, std::memory_order_relaxed);
    TrayPanningSlot().store(s.panning, std::memory_order_relaxed);
}

inline TrayStatus ReadTrayStatus() {
    TrayStatus s;
    s.level   = TrayLevelSlot().load(std::memory_order_relaxed);
    s.engine  = (TrayEngine)TrayEngineSlot().load(std::memory_order_relaxed);
    s.panning = TrayPanningSlot().load(std::memory_order_relaxed);
    return s;
}

// --- pure label logic, unit-tested ---------------------------------------------------------

inline const wchar_t* EngineLabel(TrayEngine e) {
    switch (e) {
        case TrayEngine::Transform: return L"TRANSFORM";
        case TrayEngine::Render:    return L"RENDER";
        case TrayEngine::System:    return L"SYSTEM";
        default:                    return L"ADVANCED";
    }
}

// The headline. Anything at or below 1.001x is idle - the same threshold the transform model uses
// to decide a session is live, so the tray can never claim "1.0x zoomed" while the model calls it
// idle. Zoomed levels read to one decimal: 7.4, not 7.43, which is noise at a glance.
//   out must hold at least 16 wchar_t.
inline bool FormatZoom(double level, wchar_t* out, int cap) {
    if (!out || cap < 8) return false;
    if (level <= 1.001) {
        out[0] = L'I'; out[1] = L'd'; out[2] = L'l'; out[3] = L'e'; out[4] = 0;
        return false;                       // false = idle, so the caller can style it differently
    }
    const int whole = (int)level;
    int tenth = (int)((level - whole) * 10.0 + 0.5);
    int w = whole;
    if (tenth >= 10) { tenth = 0; ++w; }    // 7.98 must render 8.0, never 7.10
    int i = 0;
    if (w >= 10) out[i++] = (wchar_t)(L'0' + (w / 10) % 10);
    out[i++] = (wchar_t)(L'0' + w % 10);
    out[i++] = L'.';
    out[i++] = (wchar_t)(L'0' + tenth);
    out[i]   = 0;
    return true;
}

}  // namespace wind
