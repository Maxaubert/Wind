// The tray flyout's main-engine dropdown, the Win32 half (WindTray.exe, issue #315). The options,
// labels and the "does this pick change anything" rule are pure and unit-tested (flyout_tools.h).
//
// `model` is read once at Wind's launch, so a pick writes it and restarts the Wind core, with no
// prompt. It is a SESSION change exactly like the Settings "Engine" row followed by its
// Restart Wind button: only the live ini is written (the active profile is not touched, so the
// Settings Save capsule shows it as unsaved), and session.keep is dropped first so the restarted
// Wind keeps that unsaved session instead of resetting the live ini to the profile
// (ResetSessionToProfile). The restart is the same one the tray's profile switch and Settings use:
// launching Wind.exe again makes the new instance evict the running one through its
// single-instance handshake, and Wind then starts a fresh tray.
#include "tray_app.h"
#include "flyout_tools.h"
#include "../logging.h"
#include "../config_path.h"
#include "../profiles_io.h"
#include "../tray_items.h"     // IniValues
#include "../config_ui/ini_edit.h"
#include <string>

namespace wind { namespace TrayApp {

namespace {
void WriteSessionKeep() {
    HANDLE h = CreateFileW(wind::SessionKeepPath().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
}
}  // namespace

bool SetMainEngine(const std::wstring& ini, int picked) {
    std::string live;
    if (GetFileAttributesW(ini.c_str()) != INVALID_FILE_ATTRIBUTES && !wind::ReadTextFileOk(ini, live)) {
        wind::Log(wind::LogLevel::Warn, "tray", "engine pick: ini unreadable (err=%lu)", GetLastError());
        Notify(L"Wind", L"Could not change the engine (config file is locked).");
        return false;
    }
    const IniValues vals = wind::ReadIniValues(live);
    auto mi = vals.find(Flyout::kEngineKey);
    const std::string oldModel = mi == vals.end() ? std::string() : mi->second;
    if (!Flyout::EnginePickChanges(Flyout::EngineIndex(oldModel), picked)) return false;

    const char* value = Flyout::EngineValue(picked);
    if (!wind::WriteTextFileAtomic(ini, wind::UpdateIniText(live, Flyout::kEngineKey, value))) {
        wind::Log(wind::LogLevel::Warn, "tray", "engine pick: ini write failed (err=%lu)", GetLastError());
        Notify(L"Wind", L"Could not change the engine (config file is locked).");
        return false;
    }
    wind::Log(wind::LogLevel::Info, "tray", "engine pick: model %s -> %s (restarting Wind)",
              oldModel.empty() ? "(unset)" : oldModel.c_str(), value);

    WriteSessionKeep();    // our own restart: the unsaved session (this very change) must survive it
    const std::wstring exe = AppDir() + L"\\Wind.exe";
    const bool haveExe = GetFileAttributesW(exe.c_str()) != INVALID_FILE_ATTRIBUTES;
    const INT_PTR rc = haveExe
        ? reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", exe.c_str(), nullptr, AppDir().c_str(),
                                                  SW_SHOWNORMAL))
        : 0;
    if (rc > 32) return true;

    // Keep "ini model == running model" (the same rule as the Settings restartFailed path and the
    // profile switch): put the old value back and drop the marker.
    wind::Log(wind::LogLevel::Warn, "tray", "engine pick: relaunch FAILED (rc=%lld haveExe=%d); reverting model",
              static_cast<long long>(rc), (int)haveExe);
    DeleteFileW(wind::SessionKeepPath().c_str());
    const std::string cur = wind::ReadTextFile(ini);
    wind::WriteTextFileAtomic(ini, oldModel.empty() ? wind::UpdateIniText(cur, Flyout::kEngineKey, "hybrid")
                                                    : wind::UpdateIniText(cur, Flyout::kEngineKey, oldModel));
    Notify(L"Wind", L"Could not restart Wind; kept the current engine.");
    return false;
}

}}  // namespace wind::TrayApp
