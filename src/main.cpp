#include <windows.h>
#include <climits>
#include <fstream>
#include "config.h"
#include "magnifier_engine.h"
#include "input_router.h"
#include "transform.h"
#include "tracker.h"
#include "zoom_controller.h"
#include "tray.h"

using namespace wind;

static InputRouter g_input;
static int g_zoomInBtnId = 2;   // XBUTTON id: 1 = XBUTTON1, 2 = XBUTTON2 (set from cfg)
static int g_zoomOutBtnId = 1;

// Set side-button state from a Raw Input transition. Mirrors the hook's id mapping so
// the two state sources are interchangeable and idempotent.
static void SetZoomButton(int xbuttonId, bool down) {
    if (xbuttonId == g_zoomInBtnId)  g_input.state().inHeld.store(down);
    if (xbuttonId == g_zoomOutBtnId) g_input.state().outHeld.store(down);
}

// Message-handler: decodes raw mouse movement (survives cursor lock) and routes
// tray messages.
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_INPUT) {
        UINT size = 0;
        GetRawInputData((HRAWINPUT)lp, RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER));
        alignas(8) BYTE buf[128];
        if (size > 0 && size <= sizeof(buf) &&
            GetRawInputData((HRAWINPUT)lp, RID_INPUT, buf, &size, sizeof(RAWINPUTHEADER)) == size) {
            auto* ri = reinterpret_cast<RAWINPUT*>(buf);
            if (ri->header.dwType == RIM_TYPEMOUSE) {
                const RAWMOUSE& m = ri->data.mouse;
                if ((m.usFlags & MOUSE_MOVE_ABSOLUTE) == 0) {
                    AccumulateRaw(g_input, m.lLastX, m.lLastY);
                }
                // Side-button state via Raw Input. This path is delivered even when an
                // elevated window (Task Manager, UAC) is foreground, where the
                // WH_MOUSE_LL hook is not invoked for a medium-IL process (UIPI). The
                // hook still runs for normal windows (and swallows the buttons there);
                // setting the same state from both sources is idempotent.
                USHORT bf = m.usButtonFlags;
                if (bf & RI_MOUSE_BUTTON_4_DOWN) SetZoomButton(1, true);
                if (bf & RI_MOUSE_BUTTON_4_UP)   SetZoomButton(1, false);
                if (bf & RI_MOUSE_BUTTON_5_DOWN) SetZoomButton(2, true);
                if (bf & RI_MOUSE_BUTTON_5_UP)   SetZoomButton(2, false);
            }
        }
        return 0;
    }
    if (Tray::HandleMessage(hwnd, msg, wp, lp)) return 0;
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
    // Single instance.
    HANDLE mtx = CreateMutexW(nullptr, TRUE, L"Wind_Magnifier_SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    // Resolve magnifier.ini next to the exe (not the launch cwd) by anchoring cwd there.
    wchar_t exePath[MAX_PATH];
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH)) {
        wchar_t* slash = wcsrchr(exePath, L'\\');
        if (slash) { *slash = L'\0'; SetCurrentDirectoryW(exePath); }
    }

    Config cfg = LoadConfig(L"magnifier.ini");
    g_zoomInBtnId = cfg.zoomInButton;
    g_zoomOutBtnId = cfg.zoomOutButton;

    // Hidden (never shown) normal window: owns the tray icon + menu and receives WM_INPUT.
    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"WindMagnifierWnd";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Wind", WS_OVERLAPPED,
                                0, 0, 0, 0, nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return 1;

    // Raw Input for the mouse, delivered even when a game is foreground.
    RAWINPUTDEVICE rid{};
    rid.usUsagePage = 0x01; rid.usUsage = 0x02; // generic mouse
    rid.dwFlags = RIDEV_INPUTSINK; rid.hwndTarget = hwnd;
    RegisterRawInputDevices(&rid, 1, sizeof(rid));

    if (!g_input.start(cfg.zoomInButton, cfg.zoomOutButton, /*swallow=*/true)) {
        MessageBoxW(nullptr, L"Failed to install the mouse hook.", L"Wind", MB_ICONERROR);
        return 1;
    }

    MagnifierEngine engine;
    if (!engine.initialize()) {
        MessageBoxW(nullptr, L"Magnification API failed to initialize.", L"Wind", MB_ICONERROR);
        g_input.stop();
        return 1;
    }
    Tray::Add(hwnd, hInst);

    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    ZoomController zoom(1.0, cfg.maxLevel, cfg.fullRangeSeconds);
    Tracker tracker(sw, sh, cfg.sensitivity);

    LARGE_INTEGER freq, prev;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&prev);
    unsigned long long lastMtime = ConfigMTime(L"magnifier.ini");
    double sinceCheck = 0.0;
    double lastLevel = -1.0; int lastX = INT_MIN, lastY = INT_MIN;
    double lastCx = -1e9, lastCy = -1e9;          // last-applied float center (updateMode 1)
    int updateMode = cfg.updateMode;
    int maxUpdateHz = (cfg.maxUpdateHz > 0) ? cfg.maxUpdateHz : 0;
    double sinceEmit = 1.0;                        // seconds since last transform emit (throttle)

    // High-resolution pacing timer (replaces DwmFlush). DwmFlush forced a DWM
    // composition pass each frame; paired with a per-frame transform change it kicked
    // borderless games out of independent-flip into composed-flip (issue #2). A plain
    // timer paces us at refresh rate without forcing composition, so the game keeps
    // its fast path while we update the overlay transform.
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr,
        CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    int hz = (cfg.tickHzCap > 0) ? cfg.tickHzCap : 144;
    LARGE_INTEGER due; due.QuadPart = -(10000000LL / hz); // relative due time, 100ns units

    // Optional frame-timing diagnostics (config: diagnostics=1) -> wind_diag.log.
    // Distinguishes a stall inside MagSetFullscreenTransform (maxSt high) from a stall
    // in our loop/wait (maxDt high, maxSt low) from a DWM-side cost (both low).
    bool diag = cfg.diagnostics != 0;
    std::ofstream diagOut;
    if (diag) {
        diagOut.open("wind_diag.log", std::ios::app);
        diagOut << "=== Wind diagnostics start (hz=" << hz << ") ===\n";
        diagOut.flush();
    }
    double winElapsed = 0.0; int winIters = 0;
    double winSumDt = 0.0, winMaxDt = 0.0;
    int winStCalls = 0; double winSumSt = 0.0, winMaxSt = 0.0;
    int winLockedTicks = 0;   // ticks the tracker treated the cursor as locked

    bool running = true;
    while (running) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { running = false; break; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!running) break;

        // Pace the loop without forcing DWM composition (see timer setup above).
        if (timer) {
            SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
            WaitForSingleObject(timer, INFINITE);
        } else {
            Sleep(1000 / hz);  // fallback (pre-Win10 1803): coarser but functional
        }

        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        double dt = double(now.QuadPart - prev.QuadPart) / double(freq.QuadPart);
        prev = now;

        // Config hot-reload (~once per second).
        sinceCheck += dt;
        if (sinceCheck > 1.0) {
            sinceCheck = 0.0;
            unsigned long long m = ConfigMTime(L"magnifier.ini");
            if (m != lastMtime) {
                lastMtime = m;
                Config nc = LoadConfig(L"magnifier.ini");
                zoom = ZoomController(1.0, nc.maxLevel, nc.fullRangeSeconds);
                tracker = Tracker(sw, sh, nc.sensitivity);
                updateMode = nc.updateMode;
                maxUpdateHz = (nc.maxUpdateHz > 0) ? nc.maxUpdateHz : 0;
            }
        }

        // Watchdog-safe: derive direction from current physical button state each tick.
        zoom.setDirection(ResolveDirection(g_input.state().inHeld.load(),
                                           g_input.state().outHeld.load()));
        zoom.tick(dt);

        if (g_input.state().recenter.exchange(false)) tracker.recenter();

        POINT p; GetCursorPos(&p);
        int dx, dy; g_input.drainRaw(dx, dy);
        tracker.update(p.x, p.y, dx, dy);

        double cx = tracker.centerX(), cy = tracker.centerY();
        double lvl = zoom.level();
        Offset o = ComputeOffset(cx, cy, lvl, sw, sh);

        // Decide whether to push the transform to DWM this frame (see updateMode docs).
        bool wantEmit;
        if (updateMode == 2) {
            wantEmit = (lvl > 1.0) || (lvl != lastLevel);          // continuous while zoomed
        } else if (updateMode == 1) {
            wantEmit = (lvl != lastLevel || cx != lastCx || cy != lastCy);
        } else {
            wantEmit = (lvl != lastLevel || o.x != lastX || o.y != lastY);
        }
        sinceEmit += dt;
        // Optional throttle (never throttle a level change, e.g. returning to 1.0x).
        if (maxUpdateHz > 0 && lvl == lastLevel && sinceEmit < 1.0 / maxUpdateHz)
            wantEmit = false;

        if (wantEmit) {
            if (diag) {
                LARGE_INTEGER a, b;
                QueryPerformanceCounter(&a);
                engine.setTransform(lvl, o.x, o.y);
                QueryPerformanceCounter(&b);
                double stMs = double(b.QuadPart - a.QuadPart) / freq.QuadPart * 1000.0;
                winStCalls++; winSumSt += stMs; if (stMs > winMaxSt) winMaxSt = stMs;
            } else {
                engine.setTransform(lvl, o.x, o.y);
            }
            lastLevel = lvl; lastX = o.x; lastY = o.y; lastCx = cx; lastCy = cy;
            sinceEmit = 0.0;
        }

        if (diag) {
            double dtMs = dt * 1000.0;
            winIters++; winSumDt += dtMs; if (dtMs > winMaxDt) winMaxDt = dtMs;
            if (tracker.locked()) winLockedTicks++;
            winElapsed += dt;
            if (winElapsed >= 2.0 && diagOut) {
                // Foreground-window title: confirms whether the game is still the
                // foreground window while we magnify (tests the "treated as bg" idea).
                char fg[96] = "?";
                if (HWND fgw = GetForegroundWindow()) {
                    wchar_t wtitle[96] = L"";
                    GetWindowTextW(fgw, wtitle, 95);
                    WideCharToMultiByte(CP_UTF8, 0, wtitle, -1, fg, sizeof(fg), nullptr, nullptr);
                }
                diagOut << "mode=" << updateMode << " cap=" << maxUpdateHz
                        << " iters=" << winIters
                        << " avgDt=" << (winIters ? winSumDt / winIters : 0.0)
                        << " maxDt=" << winMaxDt
                        << " stCalls=" << winStCalls
                        << " avgSt=" << (winStCalls ? winSumSt / winStCalls : 0.0)
                        << " maxSt=" << winMaxSt
                        << " lockedTicks=" << winLockedTicks << "/" << winIters
                        << " fg=\"" << fg << "\"\n";
                diagOut.flush();
                winElapsed = 0.0; winIters = 0; winSumDt = 0.0; winMaxDt = 0.0;
                winStCalls = 0; winSumSt = 0.0; winMaxSt = 0.0; winLockedTicks = 0;
            }
        }
    }

    if (diag && diagOut) diagOut.close();
    if (timer) CloseHandle(timer);
    engine.shutdown();   // resets to 1x - never leave the screen zoomed
    g_input.stop();
    Tray::Remove();
    ReleaseMutex(mtx);
    return 0;
}
