// Drives the LIVE deployed Wind (Mag/cursor-warp) and logs, for clicks across the screen, where Wind's
// cursor is (GetCursorPos) vs where the click actually lands (GetMessagePos in a fullscreen logger).
// If cursor==landing everywhere, clicks are correct; a band where they diverge (or where the cursor
// can't be driven) is the dead zone. Fully autonomous: injects the zoom button + mouse movement + clicks.
// Build: tools\build_test_wind_clicks.bat -> test_wind_clicks.exe. Results -> %TEMP%\wind_clicktest.txt
#include <windows.h>
#include <windowsx.h>
#include <atomic>
#include <thread>
#include <cstdio>

static std::atomic<int> g_landX{-99999}, g_landY{-99999}, g_seq{0};
static FILE* g_out = nullptr;

static LRESULT CALLBACK WP(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_LBUTTONDOWN) { DWORD mp = GetMessagePos(); g_landX.store((int)(short)LOWORD(mp)); g_landY.store((int)(short)HIWORD(mp)); g_seq.fetch_add(1); return 0; }
    return DefWindowProcW(h, m, w, l);
}
static void MoveRel(int dx, int dy) { INPUT in{}; in.type = INPUT_MOUSE; in.mi.dwFlags = MOUSEEVENTF_MOVE; in.mi.dx = dx; in.mi.dy = dy; SendInput(1, &in, sizeof(in)); }
static void XBtn(bool down) { INPUT in{}; in.type = INPUT_MOUSE; in.mi.dwFlags = down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP; in.mi.mouseData = XBUTTON2; SendInput(1, &in, sizeof(in)); }
static void Click() { INPUT in[2] = {}; in[0].type = INPUT_MOUSE; in[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN; in[1].type = INPUT_MOUSE; in[1].mi.dwFlags = MOUSEEVENTF_LEFTUP; SendInput(2, in, sizeof(INPUT)); }

static void Probe(const char* label) {
    POINT c{}; GetCursorPos(&c);
    int seq = g_seq.load();
    Click();
    Sleep(90);
    bool got = (g_seq.load() != seq);
    int lx = g_landX.load(), ly = g_landY.load();
    int dy = got ? (ly - c.y) : 0, dx = got ? (lx - c.x) : 0;
    char landed[40]; if (got) snprintf(landed, sizeof(landed), "%d,%d", lx, ly); else snprintf(landed, sizeof(landed), "MISS");
    const char* verdict = (got && abs(dx) <= 2 && abs(dy) <= 2) ? "OK" : (got ? "MISMATCH" : "no-click");
    fprintf(g_out, "%-14s cursor=(%4d,%4d)  landed=%-12s delta=(%d,%d)  %s\n", label, c.x, c.y, landed, dx, dy, verdict);
}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext((DPI_AWARENESS_CONTEXT)(-4));
    wchar_t tp[MAX_PATH]; GetTempPathW(MAX_PATH, tp); wchar_t fp[MAX_PATH]; lstrcpyW(fp, tp); lstrcatW(fp, L"wind_clicktest.txt");
    _wfopen_s(&g_out, fp, L"w"); if (!g_out) return 1;
    const int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    fprintf(g_out, "screen=%dx%d\n", sw, sh);

    WNDCLASSEXW wc{}; wc.cbSize = sizeof(wc); wc.lpfnWndProc = WP; wc.hInstance = hi;
    wc.hbrBackground = (HBRUSH)GetStockObject(DKGRAY_BRUSH); wc.lpszClassName = L"WindClickTest"; wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST, L"WindClickTest", L"t", WS_POPUP, 0, 0, sw, sh, nullptr, nullptr, hi, nullptr);
    ShowWindow(hwnd, SW_SHOW); SetForegroundWindow(hwnd);

    DWORD mainTid = GetCurrentThreadId();
    std::thread worker([&]() {
        // Launch the deployed Wind, let it start, zoom in (hold forward side-button).
        STARTUPINFOW si{ sizeof(si) }; PROCESS_INFORMATION piWind{};
        CreateProcessW(L"C:\\Program Files\\Wind\\Wind.exe", nullptr, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &piWind);
        Sleep(3500);
        SetCursorPos(sw / 2, sh / 2); Sleep(200);
        XBtn(true);                 // zoom-in (hold)
        Sleep(2500);                // let it ramp to max zoom
        POINT z{}; GetCursorPos(&z);
        fprintf(g_out, "after zoom: cursor=(%d,%d)\n", z.x, z.y);

        Probe("center");
        // Drive DOWN in steps, clicking after each, to map the bottom (where the dead band is).
        for (int step = 1; step <= 18; step++) {
            for (int k = 0; k < 8; k++) { MoveRel(0, 30); Sleep(8); }   // nudge down
            Sleep(60);
            char lbl[24]; snprintf(lbl, sizeof(lbl), "down#%d", step);
            Probe(lbl);
        }
        XBtn(false);                // release zoom
        Sleep(300);
        if (piWind.hProcess) { TerminateProcess(piWind.hProcess, 0); CloseHandle(piWind.hProcess); CloseHandle(piWind.hThread); }
        fprintf(g_out, "\nREAD: 'OK' = click landed where Wind's cursor is. 'MISMATCH' = landed elsewhere\n"
                       "(the dead zone - delta shows the error). Watch where cursorY stops climbing too.\n");
        fclose(g_out); g_out = nullptr;
        PostThreadMessageW(mainTid, WM_QUIT, 0, 0);
    });
    MSG msg; while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    worker.join();
    DestroyWindow(hwnd);
    return 0;
}
