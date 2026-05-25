// Cursor-hide spike for the own renderer (issue #4).
// Confirms MagShowSystemCursor(FALSE) hides the OS cursor without altering its shape,
// and that it restores cleanly. Visual confirmation (cursor vanished) is a human check;
// this records the API mechanics + that GetCursorInfo/the real shape survive.
//
// Build: cl /nologo /std:c++17 /EHsc /DUNICODE /D_UNICODE tools\cursorhide_spike.cpp ^
//        /Fe:cursorhide_spike.exe /link Magnification.lib user32.lib
#include <windows.h>
#include <magnification.h>
#include <cstdio>

static void report(const char* tag) {
    CURSORINFO ci{ sizeof(ci) };
    BOOL ok = GetCursorInfo(&ci);
    printf("%-22s GetCursorInfo=%d CURSOR_SHOWING=%d hCursor=%p\n",
           tag, ok, (ci.flags & CURSOR_SHOWING) ? 1 : 0, (void*)ci.hCursor);
    fflush(stdout);
}

int main() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (!MagInitialize()) { printf("MagInitialize FAILED err=%lu\n", GetLastError()); return 1; }

    report("before-hide");

    SetLastError(0);
    BOOL h1 = MagShowSystemCursor(FALSE);
    printf("MagShowSystemCursor(FALSE) ret=%d err=%lu\n", h1, GetLastError());
    fflush(stdout);
    Sleep(1500);                      // human window: is the cursor gone?
    report("during-hide(bare)");

    // Fallback path: activate the runtime with an identity transform, hide again.
    SetLastError(0);
    BOOL t = MagSetFullscreenTransform(1.0f, 0, 0);
    DWORD terr = GetLastError();
    BOOL h2 = MagShowSystemCursor(FALSE);
    printf("identity transform setT=%d err=%lu  showCursor(FALSE)=%d\n", t, terr, h2);
    fflush(stdout);
    Sleep(1500);
    report("during-hide(identity)");

    // Restore.
    MagShowSystemCursor(TRUE);
    MagSetFullscreenTransform(1.0f, 0, 0);
    MagUninitialize();
    report("after-restore");

    // Belt-and-braces: force the OS to reload visible cursors (safety net for the app).
    SystemParametersInfoW(SPI_SETCURSORS, 0, nullptr, SPIF_SENDCHANGE);
    report("after-SPI_SETCURSORS");

    printf("DONE\n");
    return 0;
}
