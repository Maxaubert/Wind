// The tray tools (WindTray.exe, issue #315): the listen-and-chime app fixes (Mouse lock, Pass keys),
// Pause Wind and the tray icon's state. The listen state outlives the flyout, which closes as soon
// as the user clicks the target app, so it lives here and not in flyout_window.cpp. The decisions
// (target filter, add or remove, cancel, 20 s timeout, pulse) are pure and unit-tested in
// flyout_tools.h; this file only wires them to a timer, the shared block, the ini files and the chime.
#include "tray_app.h"
#include "flyout_tools.h"
#include "tray_icon_badge.h"
#include "../logging.h"
#include "../config_path.h"
#include "../profiles_io.h"
#include "../config.h"
#include "../config_ui/ini_edit.h"
#include "../tray_ipc.h"
#include "../tray_items.h"
#include <string>

namespace wind { namespace TrayApp {

namespace {

const UINT_PTR kListenTimerId = 0x4C49;     // runs only while listening
const UINT kListenTickMs = 100;             // target polling; the icon pulse steps every other tick

HWND g_hwnd = nullptr;
Flyout::ListenState g_listen;
bool g_animOn = true;

bool PausedNow() { return TrayPaused(Block()); }

int PulseFrame() {
    if (!g_listen.active) return -1;
    if (!g_animOn) return kIconPulseFrames / 2;         // no animation: one steady frame
    const unsigned long long e = (unsigned long long)(GetTickCount64() - g_listen.startMs);
    return (int)((e % Flyout::kListenPulseMs) * kIconPulseFrames / Flyout::kListenPulseMs);
}

void UpdateIcon() { SetIconState(PausedNow(), PulseFrame()); }

void EndListen() {
    g_listen = Flyout::ListenState{};
    if (g_hwnd) KillTimer(g_hwnd, kListenTimerId);
    UpdateIcon();
    FlyoutRefresh();
}

// Toggles `exe` in the fix's list: live ini first (it decides add or remove), then the active
// profile (the same direction), so the change persists like a keybind. False = nothing was written.
bool ApplyAppFix(Flyout::FixKind kind, const std::string& exe, bool* added) {
    const std::wstring ini = wind::ResolveIniPath();
    std::string live;
    if (GetFileAttributesW(ini.c_str()) != INVALID_FILE_ATTRIBUTES && !wind::ReadTextFileOk(ini, live)) {
        wind::Log(wind::LogLevel::Warn, "tray", "app fix: ini unreadable (err=%lu)", GetLastError());
        return false;
    }
    const char* key = Flyout::FixKey(kind);
    IniValues vals = wind::ReadIniValues(live);
    const std::string cur = vals.count(key) ? vals[key] : std::string();
    const bool add = !Flyout::ExeListHas(cur, exe);
    const std::string next = add ? Flyout::ExeListAdd(cur, exe) : Flyout::ExeListRemove(cur, exe);
    if (!wind::WriteTextFileAtomic(ini, wind::UpdateIniText(live, key, next))) {
        wind::Log(wind::LogLevel::Warn, "tray", "app fix: ini write failed (err=%lu)", GetLastError());
        return false;
    }
    *added = add;
    // The active profile, if it has a file: the same add or remove on ITS list.
    const std::string prof = vals.count("profile") ? vals["profile"] : std::string();
    if (!prof.empty()) {
        const std::wstring pp = wind::ProfilesDirFromIni(ini) + L"\\" + wind::WidenUtf8(prof) + L".ini";
        std::string ptext;
        if (GetFileAttributesW(pp.c_str()) != INVALID_FILE_ATTRIBUTES && wind::ReadTextFileOk(pp, ptext)) {
            IniValues pv = wind::ReadIniValues(ptext);
            const std::string pcur = pv.count(key) ? pv[key] : std::string();
            const std::string pnext = add ? Flyout::ExeListAdd(pcur, exe) : Flyout::ExeListRemove(pcur, exe);
            if (!wind::WriteTextFileAtomic(pp, wind::UpdateProfileKey(ptext, key, pnext)))
                wind::Log(wind::LogLevel::Warn, "tray", "app fix: profile write failed (err=%lu)", GetLastError());
        }
    }
    return true;
}

void ListenTick() {
    TrayForeground fg;
    if (!ReadTrayForeground(Block(), fg)) { EndListen(); return; }   // Wind went away or changed layout
    const unsigned long long now = GetTickCount64();
    switch (Flyout::ListenPoll(g_listen, fg.activations, fg.exe, now)) {
        case Flyout::ListenEvent::Target: {
            const Flyout::FixKind kind = g_listen.kind;
            const std::string exe = wind::NarrowUtf8(fg.exe);
            EndListen();
            bool added = false;
            if (ApplyAppFix(kind, exe, &added)) {
                wind::Log(wind::LogLevel::Info, "tray", "app fix: %s %s %s", Flyout::FixKey(kind),
                          added ? "added" : "removed", exe.c_str());
                PlayChime(added);       // rising = enabled for that app, falling = disabled
            } else {
                Notify(L"Wind", L"Could not change that app (config file is locked).");
            }
            return;
        }
        case Flyout::ListenEvent::TimedOut:
            wind::Log(wind::LogLevel::Info, "tray", "app fix: listening timed out");
            EndListen();
            return;
        case Flyout::ListenEvent::Pending: break;
    }
    UpdateIcon();
}

}  // namespace

void ToolsInit(HWND hwnd) {
    g_hwnd = hwnd;
    BOOL on = TRUE;
    if (SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &on, 0)) g_animOn = on != FALSE;
}

void ToolsTimer(UINT_PTR id) {
    if (id == kListenTimerId && g_listen.active) ListenTick();
}

void ToolsListenClick(int fix) {
    TrayForeground fg;
    if (!ReadTrayForeground(Block(), fg)) {
        wind::Log(wind::LogLevel::Warn, "tray", "app fix: no foreground feed from Wind (older Wind?)");
        return;
    }
    {   // Re-read per click: the user may have changed the animation setting since the tray started.
        BOOL on = TRUE;
        if (SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &on, 0)) g_animOn = on != FALSE;
    }
    g_listen = Flyout::ListenClick(g_listen, fix == 1 ? Flyout::FixKind::Pass : Flyout::FixKind::Lock,
                                   fg.activations, GetTickCount64());
    if (g_hwnd) {
        if (g_listen.active) SetTimer(g_hwnd, kListenTimerId, kListenTickMs, nullptr);
        else KillTimer(g_hwnd, kListenTimerId);
    }
    UpdateIcon();
}

int ToolsListening() {
    if (!g_listen.active) return 0;
    return g_listen.kind == Flyout::FixKind::Lock ? 1 : 2;
}

unsigned long long ToolsListenElapsedMs() {
    if (!g_listen.active) return 0;
    const unsigned long long now = GetTickCount64();
    return now >= g_listen.startMs ? now - g_listen.startMs : 0;
}

void ToolsTogglePause() {
    SetPaused(!PausedNow());
    UpdateIcon();
}

}}  // namespace wind::TrayApp
