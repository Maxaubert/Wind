// `WindTray.exe --render-test out.png [--light] [--dpi N] [--hover kind[:i]]` (issue #313): renders
// the flyout with fake status to a PNG and exits, so the drawing can be compared with the j01
// references in docs/design/tray-2026-10 without a desktop session or a running Wind.
//   --light            the light theme (default dark)
//   --dpi N            scale (96 = 1x, 192 = 2x like the reference PNGs)
//   --hover K[:I]      hover state: chip:1, settings, quit, profile
// Fake data: zoom 7.4x at 144 fps, Warmth 40%, Brightness 72%, all three toggles shown (text cursor
// on, focus off, keep-within-edges on), profile "Default", Performance on.
#include "tray_app.h"
#include "flyout_draw.h"
#include "flyout_model.h"
#include "../logging.h"
#include <shellapi.h>
#include <string>

namespace wind { namespace TrayApp {

int RunRenderTest(const wchar_t*) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return 2;
    std::wstring out;
    bool light = false;
    int dpi = 96;
    Flyout::Hit hover;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--render-test" && i + 1 < argc) out = argv[++i];
        else if (a == L"--light") light = true;
        else if (a == L"--dpi" && i + 1 < argc) dpi = (std::max)(96, _wtoi(argv[++i]));
        else if (a == L"--hover" && i + 1 < argc) {
            const std::wstring k = argv[++i];
            if (k.rfind(L"chip", 0) == 0) hover = { Flyout::HitKind::Chip, k.size() > 5 ? _wtoi(k.c_str() + 5) : 0 };
            else if (k == L"settings") hover = { Flyout::HitKind::Settings, 0 };
            else if (k == L"quit") hover = { Flyout::HitKind::Quit, 0 };
            else if (k == L"profile") hover = { Flyout::HitKind::Profile, 0 };
        }
    }
    LocalFree(argv);
    if (out.empty() || !Flyout::DrawInit()) return 2;

    const IniValues ini = {
        { "trayPerf", "1" },
        { "colorWarmPct", "40" }, { "colorDimPct", "72" },
        { "trayToggles", "trackCaret,trackFocus,keepEdges" },
        { "trackCaret", "1" }, { "trackFocus", "0" }, { "mouseAlign", "1" }, { "trackAlign", "1" },
    };
    TrayStatus st;
    st.level = 7.4;
    float ticks[TickStats::kCap];
    {   // j01 shows a jagged trace, so the fake history has a few mild stalls (the sparkline only draws
        // frames above 1.5x the median); the rest is solved so the mean stays 6.94 ms (144 fps).
        static const float stall[] = { 10.1f, 10.8f, 10.4f, 11.2f, 10.2f, 10.7f, 11.0f };
        static const int gap[] = { 17, 23, 19, 27, 16, 22, 20 };
        const int N = TickStats::kCap;
        bool isStall[TickStats::kCap] = {};
        float sum = 0.f;
        int nst = 0;
        for (int i = 3, j = 0; i < N; i += gap[j % 7], ++j) { ticks[i] = stall[j % 7]; isStall[i] = true; sum += ticks[i]; ++nst; }
        const float base = (6.94f * (float)N - sum) / (float)(N - nst);
        for (int i = 0; i < N; ++i) if (!isStall[i]) ticks[i] = base;
    }
    Flyout::View v = Flyout::BuildView(ini, ParseTrayLayout(ini), st, ticks, TickStats::kCap, L"Default", !light);
    v.hover = hover;
    return Flyout::RenderToPng(v, Flyout::MeasureProfileText(v.profile), dpi, out.c_str()) ? 0 : 1;
}

int RunFlyoutTest() {
    wind::LogInit(L"tray-test");
    ToggleFlyout();
    wind::Log(wind::LogLevel::Info, "tray", "flyout-test: open=%d", (int)FlyoutIsOpen());
    const ULONGLONG end = GetTickCount64() + 20000;
    while (FlyoutIsOpen() && GetTickCount64() < end) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 50, QS_ALLINPUT);
    }
    CloseFlyout();
    return 0;
}

}}  // namespace wind::TrayApp
