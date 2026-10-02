// WindTray.exe (issue #291): owns Wind's notification-area icon and quick-controls flyout (#313),
// WITHOUT UIAccess.
//
// Lifecycle: Wind.exe starts us with `--wind-pid <pid>` (ShellExecute, so we never inherit its
// UIAccess token). We serve exactly that process and exit when it exits, removing the icon on the
// way out, so a Wind killed from Task Manager leaves no ghost icon. Single instance per session.
#include "tray_app.h"
#include "../logging.h"
#include "../tray_ipc.h"
#include <shellapi.h>
#include <cwchar>
#include <string>

namespace wind { namespace TrayApp {

static TrayShared*  g_block = nullptr;
static std::wstring g_appDir;

TrayShared* Block() { return g_block; }
std::wstring AppDir() { return g_appDir; }

void SetPaused(bool paused) {
    // Block first, then the event: Wind reads the flag when the event wakes it.
    SetTrayPaused(g_block, paused);
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, kTrayCommandEventName);
    if (ev) { SetEvent(ev); CloseHandle(ev); }
    else wind::Log(wind::LogLevel::Warn, "tray", "pause: command event open failed (err=%lu)", GetLastError());
}

void RequestWindQuit() {
    // The same clean-exit path the installer and Settings use: Wind restores cursor, clip and
    // Magnifier state as on any quit. A window message could not reach Wind anyway (UIPI).
    HANDLE ev = OpenEventW(EVENT_MODIFY_STATE, FALSE, L"Local\\Wind_QuitRequest");
    if (ev) { SetEvent(ev); CloseHandle(ev); }
    else wind::Log(wind::LogLevel::Warn, "tray", "quit: event open failed (err=%lu)", GetLastError());
}

}}  // namespace wind::TrayApp

using namespace wind;

static UINT g_taskbarCreated = 0;

static LRESULT CALLBACK TrayWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == TrayApp::WM_TRAY && (l == WM_RBUTTONUP || l == WM_LBUTTONUP)) {
        TrayApp::ToggleFlyout();
        return 0;
    }
    if (g_taskbarCreated && m == g_taskbarCreated) {
        // Explorer restarted: the shell forgot every icon. AddIcon deletes before adding, so this
        // can never leave two.
        TrayApp::AddIcon(h, GetModuleHandleW(nullptr));
        return 0;
    }
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(h, m, w, l);
}

static DWORD ParseWindPid(const wchar_t* cmd) {
    const wchar_t* p = cmd ? wcsstr(cmd, L"--wind-pid") : nullptr;
    if (!p) return 0;
    p += wcslen(L"--wind-pid");
    while (*p == L' ' || *p == L'=') ++p;
    return (DWORD)wcstoul(p, nullptr, 10);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR cmdLine, int) {
    // Direct2D / WIC need COM; the flyout lives on this (main) thread.
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    // Test hook: render the flyout with fake status to a PNG and exit (no Wind, no tray icon).
    if (cmdLine && wcsstr(cmdLine, L"--render-test")) return TrayApp::RunRenderTest(cmdLine);
    if (cmdLine && wcsstr(cmdLine, L"--flyout-test")) return TrayApp::RunFlyoutTest();
    wind::LogInit(L"tray");
    const DWORD windPid = ParseWindPid(cmdLine);

    wchar_t exe[MAX_PATH];
    if (GetModuleFileNameW(nullptr, exe, MAX_PATH)) {
        wchar_t* slash = wcsrchr(exe, L'\\');
        if (slash) { *slash = L'\0'; TrayApp::g_appDir = exe; SetCurrentDirectoryW(exe); }
    }

    // The Wind we serve. No PID or a dead PID: there is nothing to show a tray for.
    HANDLE hWind = windPid ? OpenProcess(SYNCHRONIZE, FALSE, windPid) : nullptr;
    if (!hWind) {
        wind::Log(wind::LogLevel::Warn, "tray", "no Wind process (pid=%lu err=%lu); exiting",
                  windPid, GetLastError());
        wind::LogShutdown();
        return 0;
    }

    // Single instance. On a model relaunch the old Wind's tray is still exiting when the new Wind
    // starts us, so wait for it rather than giving up (it exits as soon as its Wind is gone).
    HANDLE mtx = CreateMutexW(nullptr, FALSE, kTrayMutexName);
    if (mtx) {
        HANDLE waits[2] = { mtx, hWind };
        const DWORD w = WaitForMultipleObjects(2, waits, FALSE, 10000);
        if (w != WAIT_OBJECT_0 && w != WAIT_ABANDONED_0) {
            if (w == WAIT_OBJECT_0 + 1)
                wind::Log(wind::LogLevel::Warn, "tray",
                          "Wind pid=%lu exited before the tray mutex came free; exiting", windPid);
            else
                wind::Log(wind::LogLevel::Warn, "tray", "another tray is live (w=%lu); exiting", w);
            CloseHandle(mtx); CloseHandle(hWind);
            wind::LogShutdown();
            return 0;
        }
    }

    // The status block. Missing (an older Wind) is survivable: the header shows Idle with no fps.
    // Map the WHOLE section (size 0) and only use it if it holds our layout: an older Wind (#315)
    // made a smaller one, and reading past its end would fault. A newer Wind's larger block is fine
    // here, and its different version number makes TrayBlockValid refuse the values.
    HANDLE map = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, kTrayBlockName);
    if (map) {
        void* view = MapViewOfFile(map, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0);
        MEMORY_BASIC_INFORMATION mbi{};
        if (view && VirtualQuery(view, &mbi, sizeof(mbi)) && mbi.RegionSize >= sizeof(TrayShared))
            TrayApp::g_block = static_cast<TrayShared*>(view);
        else if (view)
            UnmapViewOfFile(view);
    }
    if (!TrayApp::g_block)
        wind::Log(wind::LogLevel::Warn, "tray", "status block unavailable (err=%lu)", GetLastError());

    WNDCLASSW wc{};
    wc.lpfnWndProc = TrayWndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"WindTrayWnd";
    RegisterClassW(&wc);
    // A hidden top-level window (not message-only): TaskbarCreated is broadcast to top-level
    // windows, and a message-only window never receives it.
    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, L"WindTrayWnd", L"Wind tray", WS_POPUP,
                                0, 0, 0, 0, nullptr, nullptr, hInst, nullptr);
    if (!hwnd) {
        wind::Log(wind::LogLevel::Error, "tray", "window create failed (err=%lu)", GetLastError());
        return 1;
    }
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    // Explorer runs at the same integrity, but allow the broadcast explicitly in case it does not.
    ChangeWindowMessageFilterEx(hwnd, g_taskbarCreated, MSGFLT_ALLOW, nullptr);
    TrayApp::AddIcon(hwnd, hInst);
    wind::Log(wind::LogLevel::Info, "tray", "serving Wind pid=%lu", windPid);

    bool running = true;
    while (running) {
        const DWORD w = MsgWaitForMultipleObjects(1, &hWind, FALSE, INFINITE, QS_ALLINPUT);
        if (w == WAIT_OBJECT_0) {
            wind::Log(wind::LogLevel::Info, "tray", "Wind exited; removing the icon");
            break;
        }
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { running = false; break; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    // Every exit path removes the icon. The flyout may still be open; ExitProcess ends it, and
    // `menuOpen` is cleared so the next Wind (or this one, restarting us) never inherits a stale
    // "open".
    TrayApp::CloseFlyout();
    TrayApp::RemoveIcon();
    SetTrayMenuOpen(TrayApp::g_block, false);
    wind::LogShutdown();
    ExitProcess(0);
}
