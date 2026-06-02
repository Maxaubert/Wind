// Measures the cursor's reachable range while a running Wind is zoomed. Injects the zoom button +
// movement to all four edges and logs GetCursorPos min/max. If MagSetInputTransform confines the
// cursor to a (zoom-shrinking) box, the range is small (= the dead zone); if free, it spans the screen.
// Build: tools\build_cursor_range.bat. Result -> %TEMP%\wind_cursor_range.txt (appends).
#include <windows.h>
#include <cstdio>
static void XBtn(bool down) { INPUT in{}; in.type = INPUT_MOUSE; in.mi.dwFlags = down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP; in.mi.mouseData = XBUTTON2; SendInput(1, &in, sizeof(in)); }
static void Move(int dx, int dy) { INPUT in{}; in.type = INPUT_MOUSE; in.mi.dwFlags = MOUSEEVENTF_MOVE; in.mi.dx = dx; in.mi.dy = dy; SendInput(1, &in, sizeof(in)); }

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR pCmd, int) {
    SetProcessDpiAwarenessContext((DPI_AWARENESS_CONTEXT)(-4));
    wchar_t tp[MAX_PATH]; GetTempPathW(MAX_PATH, tp); wchar_t fp[MAX_PATH]; lstrcpyW(fp, tp); lstrcatW(fp, L"wind_cursor_range.txt");
    FILE* out = nullptr; _wfopen_s(&out, fp, L"a"); if (!out) return 1;
    char label[64] = "run"; if (pCmd && *pCmd) { int i = 0; for (PWSTR p = pCmd; *p && i < 63; ++p) label[i++] = (char)*p; label[i] = 0; }
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    Sleep(700);
    XBtn(true);          // zoom the running Wind (forward side-button)
    Sleep(2600);
    int minx = 1 << 30, maxx = -(1 << 30), miny = 1 << 30, maxy = -(1 << 30);
    auto rec = [&]() { POINT p; GetCursorPos(&p); if (p.x < minx) minx = p.x; if (p.x > maxx) maxx = p.x; if (p.y < miny) miny = p.y; if (p.y > maxy) maxy = p.y; };
    auto sweep = [&](int dx, int dy, int n) { for (int i = 0; i < n; i++) { Move(dx, dy); Sleep(7); rec(); } };
    sweep(70, 0, 120); sweep(-70, 0, 240); sweep(70, 0, 120);   // right, left, right (X extremes)
    sweep(0, 70, 120); sweep(0, -70, 240); sweep(0, 70, 120);   // down, up, down (Y extremes)
    XBtn(false);
    int confined = (minx > 120 || maxx < sw - 120 || miny > 120 || maxy < sh - 120) ? 1 : 0;
    fprintf(out, "[%s] screen=%dx%d  reachable X[%d..%d] Y[%d..%d]  confined=%d\n", label, sw, sh, minx, maxx, miny, maxy, confined);
    fclose(out);
    return 0;
}
