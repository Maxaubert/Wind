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

static UINT DiagDoneMsg() { static UINT m = RegisterWindowMessageW(L"Wind.DiagnosticsExportDone.v1"); return m; }
static std::mutex  g_diagMx;
static std::wstring g_diagZip;
static bool g_diagOk = false, g_diagReady = false, g_diagRunning = false;

// Menu thread (2026-08-28, third design - the history matters here):
//   v1  SetTimer(8ms) kept the tick alive through TrackPopupMenu's modal loop, but SetTimer's
//       real floor is ~15.6ms, so the whole magnifier dropped to ~64Hz while any menu was open
//       (field-reported frame loss, and the runaway zoom-after-release in that degraded regime).
//   v2  a pump thread PostMessage'd WM_TIMER at 143Hz - and froze the menu solid, because POSTED
//       messages outrank HARDWARE input in the queue: the modal loop always found a posted tick
//       first and never dequeued the mouse.
//   v3  (this) the menu runs on ITS OWN THREAD with its own zero-size popup window. The main
//       thread never blocks, so the tick keeps its normal full-rate pacing with no keep-alive
//       trick at all, and the menu's input is handled by its own loop - both sides at full speed.
// One menu at a time (g_menuOpen); the weld is suspended while it is open (main.cpp reads
// MenuOpen()) so the pointer belongs to the user while they aim at menu items.
static void SwitchToProfile(const std::wstring& ini, const std::wstring& nameW);
static volatile LONG g_menuOpen = 0;
struct MenuCtx { HWND mainHwnd; POINT pt; };

static DWORD WINAPI MenuThread(LPVOID param) {
    MenuCtx ctx = *static_cast<MenuCtx*>(param);
    delete static_cast<MenuCtx*>(param);

    // TrackPopupMenu needs a window owned by the CALLING thread. Zero-size popup: never visible,
    // but a real window (not message-only) so it can take foreground for click-away dismissal.
    HWND host = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC", L"WindMenuHost",
                                WS_POPUP, ctx.pt.x, ctx.pt.y, 0, 0,
                                nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!host) { InterlockedExchange(&g_menuOpen, 0); return 0; }
    ShowWindow(host, SW_SHOWNOACTIVATE);

    const std::wstring ini = wind::ResolveIniPath();
    std::vector<std::wstring> profNames = wind::ListProfileFiles(wind::ProfilesDirFromIni(ini));
    if (profNames.size() > kMaxProfileMenuItems) profNames.resize(kMaxProfileMenuItems);
    const std::wstring active = wind::WidenUtf8(
        wind::ReadIniValues(wind::ReadTextFile(ini))["profile"]);

    HMENU m = CreatePopupMenu();
    HMENU pm = CreatePopupMenu();
    int activeIdx = -1;
    for (UINT i = 0; i < (UINT)profNames.size(); ++i) {
        if (_wcsicmp(profNames[i].c_str(), active.c_str()) == 0) activeIdx = (int)i;
        AppendMenuW(pm, MF_STRING, ID_PROFILE_BASE + i, profNames[i].c_str());
    }
    if (activeIdx >= 0)
        CheckMenuRadioItem(pm, ID_PROFILE_BASE, ID_PROFILE_BASE + (UINT)profNames.size() - 1,
                           ID_PROFILE_BASE + (UINT)activeIdx, MF_BYCOMMAND);
    AppendMenuW(m, MF_STRING, ID_SETTINGS, L"Open Settings");
    if (!profNames.empty())
        AppendMenuW(m, MF_POPUP, reinterpret_cast<UINT_PTR>(pm), L"Profiles");
    else
        DestroyMenu(pm);
    AppendMenuW(m, MF_STRING, ID_EXPORTDIAG, L"Export diagnostics");
    AppendMenuW(m, MF_STRING, ID_QUIT, L"Quit");

    SetForegroundWindow(host);   // we own the last input (the tray click), so this is permitted
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, ctx.pt.x, ctx.pt.y, 0,
                             host, nullptr);
    PostMessageW(host, WM_NULL, 0, 0);   // the documented dismiss fix
    DestroyMenu(m);
    DestroyWindow(host);

    // Actions. Everything here is safe off the main thread: ShellExecute is thread-agnostic,
    // Quit is a PostMessage to the main window, the diagnostics worker already ran off-thread
    // and completes via the guarded slot + registered message, and the profile switch is file
    // I/O whose UI feedback (Notify) is a process-global Shell_NotifyIcon call.
    if (cmd == ID_SETTINGS)
        ShellExecuteW(nullptr, L"open", L"WindConfig.exe", nullptr, nullptr, SW_SHOW);
    else if (cmd == ID_EXPORTDIAG) {
        bool start = false;
        { std::lock_guard<std::mutex> lk(g_diagMx); if (!g_diagRunning) { g_diagRunning = true; start = true; } }
        if (start) {
            HWND mainHwnd = ctx.mainHwnd;
            std::thread([mainHwnd]{
                std::wstring zip = wind::ExportDiagnosticsToDesktop();
                { std::lock_guard<std::mutex> lk(g_diagMx);
                  g_diagZip = std::move(zip); g_diagOk = !g_diagZip.empty();
                  g_diagReady = true; g_diagRunning = false; }
                PostMessageW(mainHwnd, DiagDoneMsg(), 0, 0);
            }).detach();
        }
    }
    else if (cmd == ID_QUIT)
        PostMessageW(ctx.mainHwnd, WM_CLOSE, 0, 0);
    else if (cmd >= (int)ID_PROFILE_BASE && cmd < (int)(ID_PROFILE_BASE + profNames.size()))
        SwitchToProfile(ini, profNames[cmd - ID_PROFILE_BASE]);

    InterlockedExchange(&g_menuOpen, 0);
    return 0;
}

// Diagnostics-export completion signal. The worker thread does NOT smuggle a heap pointer through the
// window message (any local process could PostMessage a forged LPARAM -> controlled deref/free). Instead
// it parks the result in this mutex-guarded slot and posts a bare wake-up; the handler reads the slot
// under the lock and never dereferences the message params. The message id is registered (process-unique,
// >= 0xC000) so it isn't a guessable WM_APP+n, and the handler ignores any wake-up with no result ready.

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

bool MenuOpen() { return InterlockedCompareExchange(&g_menuOpen, 0, 0) != 0; }

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
        // One menu at a time; a second click while it is open is just a re-click, ignored.
        if (InterlockedCompareExchange(&g_menuOpen, 1, 0) != 0) return true;
        auto* ctx = new MenuCtx{};
        ctx->mainHwnd = hwnd;
        GetCursorPos(&ctx->pt);
        HANDLE t = CreateThread(nullptr, 0, MenuThread, ctx, 0, nullptr);
        if (t) CloseHandle(t);
        else { delete ctx; InterlockedExchange(&g_menuOpen, 0); }
        return true;
    }
    if (msg == WM_CLOSE)  { DestroyWindow(hwnd); return true; }
    if (msg == WM_DESTROY) { PostQuitMessage(0); return true; }
    return false;
}
}}
