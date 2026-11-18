// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
// src/core/display_probe.cpp - see display_probe.h. Moved verbatim from main.cpp (#7).
#include "display_probe.h"

namespace wind {

// Current refresh rate (Hz) of the primary display, for pacing the idle/1x loop and the
// vsync=0 path so we don't hardcode the dev's 144Hz. Falls back to 60 if the query fails or
// reports a placeholder (some drivers report 0/1 for "hardware default").
// Refresh rate of a specific display (GDI device name, e.g. "\\.\DISPLAY2"); nullptr/empty = the
// primary/current display. Re-queried on retarget so pacing tracks the monitor we're actually on
// (a mixed-refresh multi-monitor setup would otherwise pace a 60Hz panel at the startup 144) (#74).
// Whether the output is rotated to portrait (90/270). The render engine cannot capture those
// (review 2026-10-09 M3), so the engine pick needs it before the session starts.
bool DetectRotatedOutput(const wchar_t* device) {
    DEVMODEW dm{}; dm.dmSize = sizeof(dm);
    const wchar_t* dev = (device && device[0]) ? device : nullptr;
    if (!EnumDisplaySettingsW(dev, ENUM_CURRENT_SETTINGS, &dm)) return false;
    return (dm.dmFields & DM_DISPLAYORIENTATION) &&
           (dm.dmDisplayOrientation == DMDO_90 || dm.dmDisplayOrientation == DMDO_270);
}

int DetectRefreshHz(const wchar_t* device) {
    DEVMODEW dm{}; dm.dmSize = sizeof(dm);
    const wchar_t* dev = (device && device[0]) ? device : nullptr;
    if (EnumDisplaySettingsW(dev, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)
        return (int)dm.dmDisplayFrequency;
    // 0, NOT a 60 fallback. A caller cannot otherwise tell a failed query from a real 60Hz panel,
    // and issue #232 made this run on every zoom-in rather than only on a monitor change: one
    // transient failure would have pinned a 144Hz rig to 60 for the life of the process, with every
    // tick-derived timing scaled 2.4x wrong. Callers substitute their own default.
    return 0;
}

// The primary monitor as a MonitorTarget (origin 0,0, primary size, empty device name = first
// DXGI output). This is the legacy single-monitor target and the universal fallback.
MonitorTarget PrimaryMonitor() {
    MonitorTarget t;
    t.x = 0; t.y = 0;
    t.w = GetSystemMetrics(SM_CXSCREEN);
    t.h = GetSystemMetrics(SM_CYSCREEN);
    t.device[0] = L'\0';
    return t;
}

// The monitor the cursor is currently on, as a MonitorTarget. Falls back to the primary if the
// query fails. Used at startup and on each zoom-in (when multiMonitor is on).
MonitorTarget MonitorUnderCursor() {
    POINT pt; GetCursorPos(&pt);
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFOEXW mi{}; mi.cbSize = sizeof(mi);
    if (mon && GetMonitorInfoW(mon, &mi)) {
        MonitorTarget t;
        t.x = mi.rcMonitor.left;
        t.y = mi.rcMonitor.top;
        t.w = mi.rcMonitor.right - mi.rcMonitor.left;
        t.h = mi.rcMonitor.bottom - mi.rcMonitor.top;
        lstrcpynW(t.device, mi.szDevice, 32);
        return t;
    }
    return PrimaryMonitor();
}

// Whether two targets are the same monitor (origin + size + device name).
bool SameMonitor(const MonitorTarget& a, const MonitorTarget& b) {
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h && wcscmp(a.device, b.device) == 0;
}

// True when the foreground window covers the whole target monitor - i.e. a fullscreen / borderless
// app (typically a game). Such an app is usually promoted to an independent-flip / MPO plane that
// Desktop Duplication can't see until our overlay forces DWM to composite it, which is what makes
// the first zoom-in flash the previously-focused window (issue #90). We use the bridged reveal only
// in this case so ordinary desktop zoom-ins keep the instant path.
bool ForegroundCoversMonitor(const MonitorTarget& mon) {
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    RECT wr{};
    if (!GetWindowRect(fg, &wr)) return false;
    return wr.left <= mon.x && wr.top <= mon.y &&
           wr.right >= mon.x + mon.w && wr.bottom >= mon.y + mon.h;
}

// --- Game-inspect focus steal (issue #144) ---------------------------------------------------
// A mouselook game reads the mouse via Raw Input, which no user-mode hook can block from another
// process (the documented LL-hook limitation): under Inspect the camera kept turning and the game
// fought the 1px freeze clip by recentering every frame. Backgrounding the game is the one
// user-mode lever that works - games register raw input without RIDEV_INPUTSINK and DirectInput's
// foreground cooperative level drops too, so on focus loss the camera freezes and the game
// releases its clip (exactly why Snipping Tool's overlay works over gameplay). Foreground goes to
// this invisible 1x1 helper (layered alpha 0: never painted, never seen), NOT the overlay - the
// overlay must stay WS_EX_NOACTIVATE + click-through. Wind's own pan is unaffected: raw input is
// registered with RIDEV_INPUTSINK on a message-only window and arrives regardless of foreground.
HWND g_focusStealer = nullptr;
HWND EnsureFocusStealer(const MonitorTarget& mon) {
    if (g_focusStealer && IsWindow(g_focusStealer)) {
        SetWindowPos(g_focusStealer, nullptr, mon.x, mon.y, 1, 1,
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        return g_focusStealer;
    }
    static ATOM s_atom = 0;
    if (!s_atom) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"WindFocusStealer";
        s_atom = RegisterClassW(&wc);
    }
    if (!s_atom) return nullptr;
    // No WS_EX_NOACTIVATE (it exists to accept activation); TOOLWINDOW keeps it out of the
    // taskbar and alt-tab list.
    g_focusStealer = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
                                     L"WindFocusStealer", L"Wind Inspect", WS_POPUP,
                                     mon.x, mon.y, 1, 1, nullptr, nullptr,
                                     GetModuleHandleW(nullptr), nullptr);
    if (!g_focusStealer) return nullptr;
    SetLayeredWindowAttributes(g_focusStealer, 0, 0, LWA_ALPHA);   // fully invisible
    ShowWindow(g_focusStealer, SW_SHOWNOACTIVATE);   // shown (a hidden window can't take foreground)
    return g_focusStealer;
}

// SetForegroundWindow can silently refuse under the foreground-lock rules (the signed UIAccess
// build is exempt). Verify the result, then fall back to the AttachThreadInput handshake so the
// unsigned dev build works too.
bool StealForeground(HWND w) {
    if (!w) return false;
    if (GetForegroundWindow() == w) return true;
    SetForegroundWindow(w);
    if (GetForegroundWindow() == w) return true;
    HWND fg = GetForegroundWindow();
    DWORD fgTid = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    DWORD myTid = GetCurrentThreadId();
    if (fgTid && fgTid != myTid && AttachThreadInput(myTid, fgTid, TRUE)) {
        SetForegroundWindow(w);
        AttachThreadInput(myTid, fgTid, FALSE);
    }
    return GetForegroundWindow() == w;
}

VirtualBounds QueryVirtualBounds() {
    return { GetSystemMetrics(SM_XVIRTUALSCREEN),  GetSystemMetrics(SM_YVIRTUALSCREEN),
             GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN) };
}

}  // namespace wind
