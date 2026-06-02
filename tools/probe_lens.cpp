// Tests whether Wind's cursor-warp model breaks because the OS magnifier repositions the cursor when
// the lens offset moves. Replicates Wind's exact loop: oracle (GetCursorPos delta) -> integrate ->
// SetCursorPos -> move lens to follow. Injects steady downward movement and checks whether the virtual
// cursor reaches the bottom or gets stuck (the dead band). Build: tools\build_probe_lens.bat
#include <windows.h>
#include <magnification.h>
#include <cstdio>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext((DPI_AWARENESS_CONTEXT)(-4));   // PMv2, like Wind
    wchar_t tp[MAX_PATH]; GetTempPathW(MAX_PATH, tp); wchar_t fp[MAX_PATH]; lstrcpyW(fp, tp); lstrcatW(fp, L"wind_probe_lens.txt");
    FILE* out = nullptr; _wfopen_s(&out, fp, L"w"); if (!out) return 1;
    if (!MagInitialize()) { fprintf(out, "MagInitialize failed\n"); fclose(out); return 1; }
    const int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    fprintf(out, "screen=%dx%d\n", sw, sh);

    // Test 1: does moving the lens offset reposition the cursor?
    MagSetFullscreenTransform(8.0f, 1000, 1000); Sleep(120);
    SetCursorPos(1920, 1080); Sleep(120);
    POINT g0{}; GetCursorPos(&g0);
    MagSetFullscreenTransform(8.0f, 1200, 1200); Sleep(120);
    POINT g1{}; GetCursorPos(&g1);
    fprintf(out, "TEST1 lens-move-cursor: set (1920,1080); after lens (1000,1000)->(1200,1200): g0=(%d,%d) g1=(%d,%d) cursorMoved=%d\n",
            g0.x, g0.y, g1.x, g1.y, (g0.x != g1.x || g0.y != g1.y) ? 1 : 0);

    // Test 2: replicate Wind's cursor-warp loop with a following lens, inject steady downward movement.
    const double level = 8.0; const int viewW = (int)(sw / level), viewH = (int)(sh / level);
    POINT mc{}; GetCursorPos(&mc); POINT lastSet = mc;
    MagSetFullscreenTransform((float)level, mc.x - viewW / 2, mc.y - viewH / 2); Sleep(100);
    GetCursorPos(&mc); lastSet = mc;
    fprintf(out, "TEST2 oracle loop (inject dy=+40/iter), start magCursor.y=%d:\n", mc.y);
    for (int i = 0; i < 70; i++) {
        INPUT in{}; in.type = INPUT_MOUSE; in.mi.dwFlags = MOUSEEVENTF_MOVE; in.mi.dy = 40; SendInput(1, &in, sizeof(in));
        Sleep(18);
        POINT pt{}; GetCursorPos(&pt);
        int ddx = pt.x - lastSet.x, ddy = pt.y - lastSet.y;
        mc.x += ddx; mc.y += ddy;
        if (mc.x < 0) mc.x = 0; else if (mc.x > sw - 1) mc.x = sw - 1;
        if (mc.y < 0) mc.y = 0; else if (mc.y > sh - 1) mc.y = sh - 1;
        SetCursorPos(mc.x, mc.y); lastSet = mc;
        int xOff = mc.x - viewW / 2; if (xOff < 0) xOff = 0; else if (xOff > sw - viewW) xOff = sw - viewW;
        int yOff = mc.y - viewH / 2; if (yOff < 0) yOff = 0; else if (yOff > sh - viewH) yOff = sh - viewH;
        MagSetFullscreenTransform((float)level, xOff, yOff);
        if (i % 10 == 0 || i >= 66) fprintf(out, "  iter %2d: pt=(%d,%d) ddy=%d magCursor.y=%d yOff=%d\n", i, pt.x, pt.y, ddy, mc.y, yOff);
    }
    fprintf(out, "FINAL magCursor.y=%d (target ~%d). Reached bottom -> oracle OK; stuck below -> moving lens corrupts it (the dead band).\n", mc.y, sh - 1);

    MagSetFullscreenTransform(1.0f, 0, 0); MagUninitialize(); fclose(out);
    return 0;
}
