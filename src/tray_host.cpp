#include "tray_host.h"
#include "tray_ipc.h"
#include "logging.h"
#include <windows.h>
#include <shellapi.h>
#include <objbase.h>

namespace wind { namespace TrayHost {

static HANDLE       g_map = nullptr;
static TrayShared*  g_block = nullptr;
static HANDLE       g_stop = nullptr;
static HANDLE       g_thread = nullptr;
static std::wstring g_trayExe;

// ShellExecuteEx, never CreateProcess with our token: the helper must NOT run with UIAccess (its
// whole purpose is an ordinary menu), and a shell launch gives it a plain token (verified
// 2026-09-29 with WindConfig: TokenUIAccess=0).
static HANDLE LaunchTray() {
    wchar_t params[48];
    wsprintfW(params, L"--wind-pid %lu", GetCurrentProcessId());
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC;
    sei.lpVerb = L"open";
    sei.lpFile = g_trayExe.c_str();
    sei.lpParameters = params;
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei) || !sei.hProcess) {
        wind::Log(wind::LogLevel::Warn, "tray", "WindTray launch failed (err=%lu)", GetLastError());
        return nullptr;
    }
    wind::Log(wind::LogLevel::Info, "tray", "WindTray started pid=%lu", GetProcessId(sei.hProcess));
    return sei.hProcess;
}

static DWORD WINAPI Supervisor(LPVOID) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);   // ShellExecuteEx's documented need
    ULONGLONG starts[3] = { 0, 0, 0 };   // the last three launch times, oldest first
    for (;;) {
        // Rate limit: a helper that dies on start must not spin. At most three launches in any
        // minute: a fourth waits until the oldest of the last three is a minute old.
        const ULONGLONG now = GetTickCount64();
        if (starts[0] && now - starts[0] < 60000) {
            const DWORD wait = (DWORD)(60000 - (now - starts[0]));
            wind::Log(wind::LogLevel::Warn, "tray", "WindTray restarting too often; waiting %lu ms", wait);
            if (WaitForSingleObject(g_stop, wait) == WAIT_OBJECT_0) break;
        }
        starts[0] = starts[1]; starts[1] = starts[2]; starts[2] = GetTickCount64();

        HANDLE proc = LaunchTray();
        if (!proc) {
            if (WaitForSingleObject(g_stop, 5000) == WAIT_OBJECT_0) break;
            continue;
        }
        HANDLE waits[2] = { g_stop, proc };
        const DWORD w = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        if (w == WAIT_OBJECT_0) { CloseHandle(proc); break; }
        DWORD code = 0; GetExitCodeProcess(proc, &code);
        CloseHandle(proc);
        // A tray that died with its menu open must not leave the cursor re-park suspended.
        SetTrayMenuOpen(g_block, false);
        wind::Log(wind::LogLevel::Warn, "tray", "WindTray exited (code=%lu); restarting", code);
    }
    CoUninitialize();
    return 0;
}

TrayShared* Start(const std::wstring& appDir) {
    g_map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                               (DWORD)sizeof(TrayShared), kTrayBlockName);
    if (g_map) g_block = static_cast<TrayShared*>(
        MapViewOfFile(g_map, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(TrayShared)));
    if (g_block) {
        // A mapping left open by the previous Wind (a model relaunch overlaps the two) keeps its
        // old values: clear the tray-written flag, then stamp ours.
        SetTrayMenuOpen(g_block, false);
        InitTrayBlock(*g_block, GetCurrentProcessId());
    } else {
        wind::Log(wind::LogLevel::Warn, "tray", "status block create failed (err=%lu)", GetLastError());
    }

    g_trayExe = appDir + L"\\WindTray.exe";
    if (GetFileAttributesW(g_trayExe.c_str()) == INVALID_FILE_ATTRIBUTES) {
        wind::Log(wind::LogLevel::Error, "tray", "WindTray.exe missing next to Wind.exe; no tray icon");
        return g_block;
    }
    g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (g_stop) g_thread = CreateThread(nullptr, 0, Supervisor, nullptr, 0, nullptr);
    return g_block;
}

void Stop() {
    if (g_thread) {
        SetEvent(g_stop);
        WaitForSingleObject(g_thread, 2000);
        CloseHandle(g_thread);
        g_thread = nullptr;
    }
    if (g_stop) { CloseHandle(g_stop); g_stop = nullptr; }
    // The block stays mapped until the process exits: the tick loop may still publish into it.
}

}}  // namespace wind::TrayHost
