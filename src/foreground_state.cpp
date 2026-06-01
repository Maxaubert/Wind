#include "foreground_state.h"
namespace wind {
bool RectCoversMonitor(int l, int t, int r, int b, int ml, int mt, int mr, int mb) {
    return l <= ml && t <= mt && r >= mr && b >= mb;
}
}

#ifndef WIND_TESTS
#include <windows.h>
#include <shellapi.h>
#pragma comment(lib, "shell32.lib")
namespace wind {
bool FullscreenAppForeground() {
    // Primary: Windows' own fullscreen-app signal (same one Focus Assist uses).
    QUERY_USER_NOTIFICATION_STATE st{};
    if (SUCCEEDED(SHQueryUserNotificationState(&st))) {
        if (st == QUNS_RUNNING_D3D_FULL_SCREEN || st == QUNS_PRESENTATION_MODE) return true;
    }
    // Fallback for borderless-fullscreen games: the foreground window covers its monitor and is
    // not the shell/desktop. Bias toward "yes" when uncertain (a false yes only costs a little
    // desktop GPU; a false no throttles a real game).
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    wchar_t cls[64] = {};
    GetClassNameW(fg, cls, 64);
    // Shell/desktop windows that are full-screen but are NOT games.
    if (!lstrcmpW(cls, L"Progman") || !lstrcmpW(cls, L"WorkerW") ||
        !lstrcmpW(cls, L"Shell_TrayWnd") || !lstrcmpW(cls, L"WindRenderOverlay") ||
        !lstrcmpW(cls, L"WindMagnifierWnd")) return false;
    RECT wr;
    if (!GetWindowRect(fg, &wr)) return false;
    HMONITOR mon = MonitorFromWindow(fg, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{}; mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) return false;
    return RectCoversMonitor(wr.left, wr.top, wr.right, wr.bottom,
                             mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right, mi.rcMonitor.bottom);
}
}
#endif
