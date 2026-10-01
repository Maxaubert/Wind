// What the tray flyout's buttons do (WindTray.exe, issue #291 / #313): the theme choice, the Quit
// guard, the profile switch and opening Settings. No UI of its own; the flyout (flyout_window.cpp)
// owns the window. The old owner-drawn HMENU that lived here is gone (menus cannot host sliders).
#include "tray_app.h"
#include "../logging.h"
#include "../config_path.h"
#include "../profiles_io.h"
#include "../config.h"
#include "../config_ui/ini_edit.h"
#include "../tray_ipc.h"
#include <shellapi.h>
#include <commctrl.h>
#pragma comment(lib, "comctl32.lib")
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#include <string>
namespace wind { namespace TrayApp {

static bool SystemUsesLightTheme() {
    DWORD v = 0, cb = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER,
                     L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &cb) == ERROR_SUCCESS)
        return v != 0;
    return false;   // no key: assume dark, which is what Wind's own UI defaults to
}

// Dark or light for this open: the ini's uiTheme ("dark" / "light" / anything else = auto, which
// follows the system). Read fresh on every open so a theme change in Settings shows immediately.
bool UsesDarkTheme(const std::string& iniText) {
    auto vals = wind::ReadIniValues(iniText);
    auto it = vals.find("uiTheme");
    const std::string t = it == vals.end() ? std::string() : it->second;
    if (t == "dark") return true;
    if (t == "light") return false;
    return !SystemUsesLightTheme();
}

void OpenSettings() {
    ShellExecuteW(nullptr, L"open", (AppDir() + L"\\WindConfig.exe").c_str(), nullptr,
                  AppDir().c_str(), SW_SHOW);
}

// Quit guard: the tray decides from the files (live ini vs the active profile), never from the UI,
// so it still prompts when Settings is closed. Returns false when the user cancels the quit.
bool ConfirmQuit(const std::wstring& ini) {
    const std::string live = wind::ReadTextFile(ini);
    auto vals = wind::ReadIniValues(live);
    auto it = vals.find("profile");
    if (it == vals.end() || it->second.empty()) return true;
    const std::wstring pp = wind::ProfilesDirFromIni(ini) + L"\\" + wind::WidenUtf8(it->second) + L".ini";
    std::string profile;
    if (!wind::ReadTextFileOk(pp, profile)) return true;   // no saved state to compare against
    if (!wind::SessionDiffers(live, profile)) return true;

    enum { ID_SAVE = 2001, ID_DISCARD = 2002 };
    const TASKDIALOG_BUTTON buttons[] = { { ID_SAVE, L"Save" }, { ID_DISCARD, L"Discard" } };
    TASKDIALOGCONFIG cfg{};
    cfg.cbSize = sizeof(cfg);
    cfg.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION;
    cfg.dwCommonButtons = TDCBF_CANCEL_BUTTON;
    cfg.pszWindowTitle = L"Wind";
    cfg.pszMainInstruction = L"You have unsaved settings";
    cfg.pszContent = L"Save them to the active profile before quitting, or discard them?";
    cfg.cButtons = 2;
    cfg.pButtons = buttons;
    cfg.nDefaultButton = ID_SAVE;
    int pressed = 0;
    if (FAILED(TaskDialogIndirect(&cfg, &pressed, nullptr, nullptr))) return true;
    if (pressed == ID_SAVE) {
        if (!wind::WriteTextFileAtomic(pp, wind::MakeProfileText(live))) {
            wind::Log(wind::LogLevel::Warn, "profile", "quit save failed (err=%lu)", GetLastError());
            Notify(L"Wind", L"Could not save the settings; Wind keeps running.");
            return false;
        }
        return true;
    }
    return pressed == ID_DISCARD;
}
// Switch the active profile from the tray: rewrite the live ini from the profile file (globals
// preserved); the core's dir-watch hot-reloads everything except `model`, which is read once at
// launch - a model change relaunches Wind.exe from our folder (the new instance evicts the running
// one via the single-instance handshake, same as the swap-model path in Wind's main.cpp).
void SwitchToProfile(const std::wstring& ini, const std::wstring& nameW) {
    // Every step logs (issue #184: a field switch failed with no trace - the balloon is
    // transient, the log is not).
    wind::Log(wind::LogLevel::Info, "profile", "tray switch -> %ls", nameW.c_str());
    const std::wstring profPath = wind::ProfilesDirFromIni(ini) + L"\\" + nameW + L".ini";
    if (GetFileAttributesW(profPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        wind::Log(wind::LogLevel::Warn, "profile", "switch aborted: file missing");
        Notify(L"Wind", L"That profile's file is missing; settings unchanged.");
        return;
    }
    // A read failure must NOT masquerade as an empty profile: empty legitimately means factory
    // defaults, so silently treating a locked/corrupt file as empty would wipe the live settings.
    std::string profText;
    if (!wind::ReadTextFileOk(profPath, profText)) {
        wind::Log(wind::LogLevel::Warn, "profile", "switch aborted: read failed (err=%lu)", GetLastError());
        Notify(L"Wind", L"Could not read that profile's file; settings unchanged.");
        return;
    }
    { std::string terr = wind::ProfileTextError(profText);
      if (!terr.empty()) {
        wind::Log(wind::LogLevel::Warn, "profile", "switch aborted: %s", terr.c_str());
        Notify(L"Wind", L"That profile's file looks corrupt; settings unchanged.");
        return;
    } }
    const std::string oldLive = wind::ReadTextFile(ini);
    // Capture hand edits (openIni) into the outgoing profile before the live ini is replaced.
    wind::MirrorLiveToActiveProfile(ini, oldLive);
    const std::string newLive = wind::MakeLiveText(profText, oldLive, wind::NarrowUtf8(nameW));
    if (!wind::WriteTextFileAtomic(ini, newLive)) {
        wind::Log(wind::LogLevel::Warn, "profile", "switch aborted: live ini write failed (err=%lu)", GetLastError());
        Notify(L"Wind", L"Could not switch profile (config file is locked).");
        return;
    }
    // Model is not hot-swappable: relaunch so the new instance boots on the profile's model.
    const std::string oldModel = wind::ParseConfig(oldLive).model;
    const std::string newModel = wind::ParseConfig(newLive).model;
    wind::Log(wind::LogLevel::Info, "profile", "switch applied: model %s -> %s%s",
              oldModel.c_str(), newModel.c_str(),
              oldModel != newModel ? " (relaunching)" : " (hot)");
    if (oldModel != newModel) {
        // Wind.exe, NOT our own exe (GetModuleFileNameW here is WindTray.exe).
        const std::wstring exe = AppDir() + L"\\Wind.exe";
        const bool haveExe = GetFileAttributesW(exe.c_str()) != INVALID_FILE_ATTRIBUTES;
        INT_PTR rc = haveExe
            ? reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", exe.c_str(), nullptr,
                                                      AppDir().c_str(), SW_SHOWNORMAL))
            : 0;
        if (rc > 32) {
            Notify(L"Wind", (L"Switched to \"" + nameW + L"\" (restarting for its model).").c_str());
        } else {
            // Keep the "ini model == running model" invariant (same rule as the Settings
            // restartFailed path): the switch stays, only the model is kept.
            wind::Log(wind::LogLevel::Warn, "profile",
                      "relaunch FAILED (rc=%lld haveExe=%d); reverting model to %s",
                      static_cast<long long>(rc), (int)haveExe, oldModel.c_str());
            wind::WriteTextFileAtomic(ini, wind::UpdateIniText(newLive, "model", oldModel));
            Notify(L"Wind", L"Profile switched; kept the current model (restart failed).");
        }
    }
}

}}  // namespace wind::TrayApp
