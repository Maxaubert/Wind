// The tray flyout's main-engine dropdown, the Win32 half (WindTray.exe, issue #315). The options,
// labels and the "does this pick change anything" rule are pure and unit-tested (flyout_tools.h).
//
// A pick writes `model`. A Wind with both engines up (running.model "live") applies it on the config
// reload, nothing else happens. One with a single engine left reads `model` once at launch, so then
// the pick also restarts the Wind core, with no prompt. It is a SESSION change exactly like the Settings "Engine" row followed by its
// Restart Wind button: only the live ini is written (the active profile is not touched, so the
// Settings Save capsule shows it as unsaved), and session.keep is dropped first so the restarted
// Wind keeps that unsaved session instead of resetting the live ini to the profile
// (ResetSessionToProfile). The restart is the same one the tray's profile switch and Settings use:
// launching Wind.exe again makes the new instance evict the running one through its
// single-instance handshake, and Wind then starts a fresh tray.
#include "tray_app.h"
#include "flyout_tools.h"
#include "logging.h"
#include "config_path.h"
#include "profiles_io.h"
#include "profile_switch.h"   // WriteSessionKeep, LaunchWindExe, RevertModelAfterFailedRelaunch
#include "tray_items.h"     // IniValues
#include "ini_edit.h"
#include <string>

namespace wind { namespace TrayApp {

bool SetMainEngine(const std::wstring& ini, int picked, const std::map<std::string, std::string>& carry) {
    std::string oldModel;
    const char* value = Flyout::EngineValue(picked);
    {
        // The whole read-modify-write under the shared ini lock, so a Settings write cannot slip in
        // between and be lost (review item 71).
        wind::IniWriteLock lock;
        std::string live;
        if (!wind::ReadLiveIni(ini, live)) {
            wind::Log(wind::LogLevel::Warn, "tray", "engine pick: ini unreadable (err=%lu)", GetLastError());
            Notify(L"Wind", L"Could not change the engine (config file is locked).");
            return false;
        }
        const IniValues vals = wind::ReadIniValues(live);
        auto mi = vals.find(Flyout::kEngineKey);
        oldModel = mi == vals.end() ? std::string() : mi->second;
        for (const auto& kv : carry) live = wind::UpdateIniText(live, kv.first, kv.second);
        if (!Flyout::EnginePickChanges(Flyout::EngineIndex(oldModel), picked)) {
            if (!carry.empty()) wind::WriteTextFileAtomic(ini, live);
            return false;
        }
        if (!wind::WriteTextFileAtomic(ini, wind::UpdateIniText(live, Flyout::kEngineKey, value))) {
            wind::Log(wind::LogLevel::Warn, "tray", "engine pick: ini write failed (err=%lu)", GetLastError());
            Notify(L"Wind", L"Could not change the engine (config file is locked).");
            return false;
        }
    }
    if (wind::EngineSwitchesLive()) {
        wind::Log(wind::LogLevel::Info, "tray", "engine pick: model %s -> %s (applies live)",
                  oldModel.empty() ? "(unset)" : oldModel.c_str(), value);
        return true;
    }
    wind::Log(wind::LogLevel::Info, "tray", "engine pick: model %s -> %s (restarting Wind)",
              oldModel.empty() ? "(unset)" : oldModel.c_str(), value);

    wind::WriteSessionKeep();    // our own restart: the unsaved session (this very change) must survive it
    const INT_PTR rc = wind::LaunchWindExe();
    if (rc > 32) return true;

    // Keep "ini model == running model" (the same rule as the Settings restartFailed path and the
    // profile switch): put the old value back and drop the marker.
    wind::Log(wind::LogLevel::Warn, "tray", "engine pick: relaunch FAILED (rc=%lld); reverting model",
              static_cast<long long>(rc));
    wind::RevertModelAfterFailedRelaunch(ini, oldModel, nullptr);
    Notify(L"Wind", L"Could not restart Wind; kept the current engine.");
    return false;
}

}}  // namespace wind::TrayApp
