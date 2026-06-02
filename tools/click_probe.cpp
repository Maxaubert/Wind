// Autonomous click-landing probe for the Mag-mode dead-zone. Creates a fullscreen window that logs
// where each injected click is actually delivered (GetMessagePos at WM_LBUTTONDOWN), while we drive
// MagSetFullscreenTransform with KNOWN parameters and inject clicks down a vertical line. Comparing
// "aimed screen Y" vs "landed screen Y" reveals the exact input mapping and pinpoints the dead band -
// no human needed. Tests with the input transform OFF (the cursor-warp case) by default.
//
// Build: tools\build_click_probe.bat -> click_probe.exe (windowed). Results -> %TEMP%\wind_click_probe.txt
#include <windows.h>
#include <windowsx.h>
#include <magnification.h>
#include <atomic>
#include <thread>
#include <string>
#include <cstdio>

static std::atomic<int> g_landX{-1}, g_landY{-1}, g_seq{0};
static FILE* g_out = nullptr;

static LRESULT CALLBACK WP(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_LBUTTONDOWN) {
        DWORD mp = GetMessagePos();
        g_landX.store((int)(short)LOWORD(mp));
        g_landY.store((int)(short)HIWORD(mp));
        g_seq.fetch_add(1);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static void Click() {
    INPUT in[2] = {};
    in[0].type = INPUT_MOUSE; in[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    in[1].type = INPUT_MOUSE; in[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput(2, in, sizeof(INPUT));
}

// One sweep at a given level, lens clamped to the BOTTOM (yOff = sh-viewH), clicking down a vertical
// line. aimedY vs landedY exposes where the OS magnifier sends clicks vs where the cursor visibly is.
static void Sweep(double level) {
    const int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    const int viewW = (int)(sw / level), viewH = (int)(sh / level);
    const int xOff = (sw - viewW) / 2;       // horizontally centered
    const int yOff = sh - viewH;             // clamped to the bottom (where the user sees the dead band)
    MagSetFullscreenTransform((float)level, xOff, yOff);
    Sleep(400);
    fprintf(g_out, "\n=== level=%.1f  view=%dx%d  lens offset=(%d,%d)  [bottom-clamped] ===\n",
            level, viewW, viewH, xOff, yOff);
    fprintf(g_out, "%-8s %-9s %-9s %-12s %-9s\n", "aimY", "cursorY", "landedY", "expect(src)", "got");
    const int tx = sw / 2;
    for (int aimY = sh - 600; aimY <= sh - 1; aimY += 40) {   // sweep the bottom 600px finely
        if (aimY < 0) continue;
        SetCursorPos(tx, aimY);
        Sleep(50);
        POINT g{}; GetCursorPos(&g);
        int seq = g_seq.load();
        Click();
        Sleep(70);
        bool got = (g_seq.load() != seq);
        // If the magnifier maps the click to the magnified content under the cursor, the landed point
        // should be the source position shown at screen (tx,aimY): src = (xOff,yOff) + (aimX,aimY)/level.
        int expSrcY = yOff + (int)(aimY / level);
        char landed[32]; if (got) snprintf(landed, sizeof(landed), "%d", g_landY.load()); else snprintf(landed, sizeof(landed), "MISS");
        fprintf(g_out, "%-8d %-9d %-9s %-12d %-9s\n", aimY, g.y, landed, expSrcY, got ? "yes" : "NO");
    }
    MagSetFullscreenTransform(1.0f, 0, 0);
    Sleep(200);
}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR, int) {
    // Match Wind: Per-Monitor-V2 DPI awareness, so GetSystemMetrics/GetCursorPos are PHYSICAL pixels
    // and MagSetFullscreenTransform offsets are in the same space Wind uses. (DPI_AWARENESS_CONTEXT
    // _PER_MONITOR_AWARE_V2 == (HANDLE)-4.) Must be set before any window/metrics call.
    SetProcessDpiAwarenessContext((DPI_AWARENESS_CONTEXT)(-4));
    wchar_t tp[MAX_PATH]; DWORD n = GetTempPathW(MAX_PATH, tp); wchar_t fp[MAX_PATH];
    if (n && n < MAX_PATH) { lstrcpyW(fp, tp); lstrcatW(fp, L"wind_click_probe.txt"); } else lstrcpyW(fp, L"wind_click_probe.txt");
    _wfopen_s(&g_out, fp, L"w");
    if (!g_out) return 1;
    if (!MagInitialize()) { fprintf(g_out, "MagInitialize failed\n"); fclose(g_out); return 1; }

    const int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    WNDCLASSEXW wc{}; wc.cbSize = sizeof(wc); wc.lpfnWndProc = WP; wc.hInstance = hi;
    wc.hbrBackground = (HBRUSH)GetStockObject(DKGRAY_BRUSH); wc.lpszClassName = L"WindClickProbe";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST, L"WindClickProbe", L"probe", WS_POPUP, 0, 0, sw, sh, nullptr, nullptr, hi, nullptr);
    if (!hwnd) { fprintf(g_out, "window failed\n"); fclose(g_out); MagUninitialize(); return 1; }
    ShowWindow(hwnd, SW_SHOW); SetForegroundWindow(hwnd); SetFocus(hwnd);

    DWORD mainTid = GetCurrentThreadId();
    std::thread worker([&]() {
        Sleep(600);
        Sweep(4.0);
        Sweep(8.0);
        Sweep(16.0);
        fprintf(g_out, "\nREAD: 'landedY' is where the OS delivered the click. If it tracks 'cursorY'\n"
                       "(or 'expect(src)') smoothly, clicks map correctly; a band where landedY jumps\n"
                       "away or 'got'=NO is the dead zone, and its pattern tells us the mechanism.\n");
        fclose(g_out); g_out = nullptr;
        PostThreadMessageW(mainTid, WM_QUIT, 0, 0);
    });
    MSG msg; while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    worker.join();
    MagSetFullscreenTransform(1.0f, 0, 0);
    MagUninitialize();
    DestroyWindow(hwnd);
    return 0;
}
