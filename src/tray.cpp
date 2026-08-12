#include "tray.h"
#include "resource.h"
#include "logging.h"
#include "config_path.h"
#include "profiles_io.h"
#include "config.h"
#include "config_ui/ini_edit.h"
#include <shellapi.h>
#include <string>
#include <thread>
#include <mutex>
#include <vector>
namespace wind { namespace Tray {
static NOTIFYICONDATAW g_nid{};
static const UINT WM_TRAY = WM_APP + 1;
static const UINT ID_SETTINGS = 1003, ID_QUIT = 1002;
static const UINT ID_EXPORTDIAG = 1004;
static const UINT ID_PROFILE_BASE = 1100;      // 1100..1131: one per profile menu item
static const UINT kMaxProfileMenuItems = 32;

// Diagnostics-export completion signal. The worker thread does NOT smuggle a heap pointer through the
// window message (any local process could PostMessage a forged LPARAM -> controlled deref/free). Instead
// it parks the result in this mutex-guarded slot and posts a bare wake-up; the handler reads the slot
// under the lock and never dereferences the message params. The message id is registered (process-unique,
// >= 0xC000) so it isn't a guessable WM_APP+n, and the handler ignores any wake-up with no result ready.
static UINT DiagDoneMsg() { static UINT m = RegisterWindowMessageW(L"Wind.DiagnosticsExportDone.v1"); return m; }
static std::mutex  g_diagMx;
static std::wstring g_diagZip;
static bool g_diagOk = false, g_diagReady = false, g_diagRunning = false;

void Add(HWND hwnd, HINSTANCE hInst) {
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY;
    // Our logo badge at the shell's small-icon size (picks the 16px frame from the multi-size .ico
    // for a crisp tray render). Fall back to the generic app icon if the resource can't be loaded.
    if (!hInst) hInst = GetModuleHandleW(nullptr);
    g_nid.hIcon = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_WIND), IMAGE_ICON,
                                    GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
                                    LR_DEFAULTCOLOR);
    if (!g_nid.hIcon) g_nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    lstrcpyW(g_nid.szTip, L"Wind magnifier");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}
void Remove() { Shell_NotifyIconW(NIM_DELETE, &g_nid); }
void Notify(const wchar_t* title, const wchar_t* text) {
    g_nid.uFlags = NIF_INFO;
    // Bounded copies: szInfoTitle is 64 wchars, szInfo 256; profile names travel through here,
    // so an unbounded lstrcpyW was a caller-controlled overflow of the fixed NOTIFYICONDATA.
    lstrcpynW(g_nid.szInfoTitle, title, ARRAYSIZE(g_nid.szInfoTitle));
    lstrcpynW(g_nid.szInfo, text, ARRAYSIZE(g_nid.szInfo));
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
}
// Switch the active profile from the tray: rewrite the live ini from the profile file (globals
// preserved); the core's dir-watch hot-reloads everything except `model`, which is read once at
// launch - a model change relaunches Wind.exe (the new instance evicts us via the single-instance
// handshake, same as the swap-model path in main.cpp).
static void SwitchToProfile(const std::wstring& ini, const std::wstring& nameW) {
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
        wchar_t exe[MAX_PATH];
        const bool haveExe = GetModuleFileNameW(nullptr, exe, MAX_PATH) != 0;
        INT_PTR rc = haveExe
            ? reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", exe, nullptr, nullptr,
                                                      SW_SHOWNORMAL))
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

bool HandleMessage(HWND hwnd, UINT msg, WPARAM /*wp*/, LPARAM lp) {
    if (msg == DiagDoneMsg()) {
        // Export worker finished (off-thread). Read the result from the guarded slot - the message params
        // are NOT trusted/dereferenced, so a forged wake-up from another process can't deref a pointer.
        std::wstring zip; bool ok = false, ready = false;
        { std::lock_guard<std::mutex> lk(g_diagMx);
          if (g_diagReady) { zip = std::move(g_diagZip); ok = g_diagOk; g_diagReady = false; g_diagZip.clear(); ready = true; } }
        if (!ready) return true;   // no export result pending (spurious/foreign wake-up): ignore
        if (ok && !zip.empty()) {  // reveal + notify here, on the message thread (tray state stays single-threaded)
            std::wstring args = L"/select,\"" + zip + L"\"";
            ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
            Notify(L"Wind", L"Diagnostics exported to your Desktop.");
        } else {
            Notify(L"Wind", L"Could not export diagnostics.");
        }
        return true;
    }
    if (msg == WM_TRAY && (lp == WM_RBUTTONUP || lp == WM_LBUTTONUP)) {
        POINT pt; GetCursorPos(&pt);
        HMENU m = CreatePopupMenu();
        // Profiles submenu: enumerated fresh on every open so external edits show up. Radio-checked
        // active entry; capped at 32 (IDs 1100..1131). Management (rename/duplicate/delete) lives in
        // the Settings UI only.
        const std::wstring ini = wind::ResolveIniPath();
        std::vector<std::wstring> profNames = wind::ListProfileFiles(wind::ProfilesDirFromIni(ini));
        if (profNames.size() > kMaxProfileMenuItems) profNames.resize(kMaxProfileMenuItems);
        const std::wstring active = wind::WidenUtf8(
            wind::ReadIniValues(wind::ReadTextFile(ini))["profile"]);
        HMENU pm = CreatePopupMenu();
        int activeIdx = -1;
        for (UINT i = 0; i < (UINT)profNames.size(); ++i) {
            if (_wcsicmp(profNames[i].c_str(), active.c_str()) == 0) activeIdx = (int)i;
            AppendMenuW(pm, MF_STRING, ID_PROFILE_BASE + i, profNames[i].c_str());
        }
        if (activeIdx >= 0)   // radio bullet (not a checkmark) on the active profile
            CheckMenuRadioItem(pm, ID_PROFILE_BASE, ID_PROFILE_BASE + (UINT)profNames.size() - 1,
                               ID_PROFILE_BASE + (UINT)activeIdx, MF_BYCOMMAND);
        AppendMenuW(m, MF_STRING, ID_SETTINGS, L"Open Settings");
        if (!profNames.empty())   // second entry, right under Open Settings
            AppendMenuW(m, MF_POPUP, reinterpret_cast<UINT_PTR>(pm), L"Profiles");
        else
            DestroyMenu(pm);   // no profiles dir yet (pre-migration): keep the menu as before
        AppendMenuW(m, MF_STRING, ID_EXPORTDIAG, L"Export diagnostics");
        AppendMenuW(m, MF_STRING, ID_QUIT, L"Quit");
        SetForegroundWindow(hwnd);  // required so the menu dismisses on click-away
        // TrackPopupMenu runs its own modal message loop that owns the thread until it closes.
        // A timer keeps WM_TIMER (and thus the magnifier tick in WndProc) firing through it, so
        // the zoom doesn't freeze while the menu is open. ~8 ms is clamped to the system minimum.
        UINT_PTR tickTimer = SetTimer(hwnd, 0xC001, 8, nullptr);
        int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
        if (tickTimer) KillTimer(hwnd, 0xC001);
        PostMessageW(hwnd, WM_NULL, 0, 0);  // the documented dismiss fix
        DestroyMenu(m);
        if (cmd == ID_SETTINGS)
            ShellExecuteW(nullptr, L"open", L"WindConfig.exe", nullptr, nullptr, SW_SHOW);
        else if (cmd == ID_EXPORTDIAG) {
            // Run the zip OFF the message thread: ExportDiagnosticsToDesktop spawns powershell and waits
            // up to 30s, and this WndProc is the same thread that drives the magnifier tick - doing it
            // inline froze the overlay for the whole export. The worker parks the result in g_diag* and
            // posts a bare wake-up (no pointer); the handler consumes it. Coalesce re-clicks while running.
            bool start = false;
            { std::lock_guard<std::mutex> lk(g_diagMx); if (!g_diagRunning) { g_diagRunning = true; start = true; } }
            if (start) {
                std::thread([hwnd]{
                    std::wstring zip = wind::ExportDiagnosticsToDesktop();
                    { std::lock_guard<std::mutex> lk(g_diagMx);
                      g_diagZip = std::move(zip); g_diagOk = !g_diagZip.empty();
                      g_diagReady = true; g_diagRunning = false; }
                    PostMessageW(hwnd, DiagDoneMsg(), 0, 0);   // wake-up only; params carry nothing
                }).detach();
            }
        }
        else if (cmd == ID_QUIT)
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
        else if (cmd >= (int)ID_PROFILE_BASE && cmd < (int)(ID_PROFILE_BASE + profNames.size()))
            SwitchToProfile(ini, profNames[cmd - ID_PROFILE_BASE]);
        return true;
    }
    if (msg == WM_CLOSE)  { DestroyWindow(hwnd); return true; }
    if (msg == WM_DESTROY) { PostQuitMessage(0); return true; }
    return false;
}
}}
