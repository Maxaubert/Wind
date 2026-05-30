// Separate-process click target for the present spike. Creates a visible topmost window whose
// whole client area is a "button"; logs a QPC timestamp on every left button down/up to
// %TEMP%\present_spike_probe.log, and writes its client rect (screen coords) to
// %TEMP%\present_spike_probe_rect.txt so the harness knows where to click. Being a DIFFERENT
// process is what makes the harness's click-through test valid. Run: clickprobe.exe [--seconds N]
#include "spike_common.h"
#include <cstring>
#include <cstdlib>

static const char* kProbeLog  = "present_spike_probe.log";
static const char* kProbeRect = "present_spike_probe_rect.txt";

static LRESULT CALLBACK ProbeProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_LBUTTONDOWN: spike::LogLine(kProbeLog, "%lld DOWN", spike::QpcNow()); return 0;
        case WM_LBUTTONUP:   spike::LogLine(kProbeLog, "%lld UP",   spike::QpcNow()); return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
            RECT rc; GetClientRect(h, &rc);
            HBRUSH b = CreateSolidBrush(RGB(0, 160, 0));
            FillRect(dc, &rc, b); DeleteObject(b);
            EndPaint(h, &ps); return 0;
        }
        case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

int main(int argc, char** argv) {
    int seconds = 0;
    for (int i = 1; i < argc; ++i)
        if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) seconds = atoi(argv[++i]);

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    spike::DeleteTemp(kProbeLog);   // start clean each run

    WNDCLASSW wc{}; wc.lpfnWndProc = ProbeProc; wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"PresentSpikeProbe"; wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);

    const int x = 300, y = 300, w = 400, h = 400;
    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST, wc.lpszClassName, L"present-spike probe",
        WS_OVERLAPPEDWINDOW, x, y, w, h, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) return 1;
    ShowWindow(hwnd, SW_SHOWNORMAL);
    SetForegroundWindow(hwnd);

    // Publish the client-area rect in screen coordinates so the harness can click its center.
    RECT cr; GetClientRect(hwnd, &cr);
    POINT tl{ cr.left, cr.top }, br{ cr.right, cr.bottom };
    ClientToScreen(hwnd, &tl); ClientToScreen(hwnd, &br);
    spike::WriteLine(kProbeRect, "%ld %ld %ld %ld", tl.x, tl.y, br.x, br.y);

    long long deadline = seconds > 0 ? spike::QpcNow() + (long long)seconds * spike::QpcFreq() : 0;
    MSG msg;
    for (;;) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) return 0;
            TranslateMessage(&msg); DispatchMessageW(&msg);
        }
        if (deadline && spike::QpcNow() >= deadline) break;
        Sleep(5);
    }
    return 0;
}
