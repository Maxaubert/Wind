#include "tray.h"
#include "resource.h"
#include "logging.h"
#include <shellapi.h>
#include <string>
#include <thread>
#include <mutex>
namespace wind { namespace Tray {
static NOTIFYICONDATAW g_nid{};
static const UINT WM_TRAY = WM_APP + 1;
static const UINT ID_SETTINGS = 1003, ID_QUIT = 1002;
static const UINT ID_EXPORTDIAG = 1004;

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
    lstrcpyW(g_nid.szInfoTitle, title);
    lstrcpyW(g_nid.szInfo, text);
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
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
        AppendMenuW(m, MF_STRING, ID_SETTINGS, L"Open Settings");
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
        return true;
    }
    if (msg == WM_CLOSE)  { DestroyWindow(hwnd); return true; }
    if (msg == WM_DESTROY) { PostQuitMessage(0); return true; }
    return false;
}
}}
