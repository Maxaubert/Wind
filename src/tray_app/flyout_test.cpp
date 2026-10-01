// `WindTray.exe --render-test out.png [--light] [--dpi N] [--hover kind[:i]]` (issue #313): renders
// the flyout with fake status to a PNG and exits, so the drawing can be compared with the j01
// references in docs/design/tray-2026-10 without a desktop session or a running Wind.
//   --light            the light theme (default dark)
//   --dpi N            scale (96 = 1x, 192 = 2x like the reference PNGs)
//   --hover K[:I]      hover state: chip:1, settings, quit, profile
// Fake data: zoom 7.4x at 144 fps, Warmth 40%, Brightness 72%, all three toggles shown (text cursor
// on, focus off, keep-within-edges off), profile "Default", Performance on.
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
        { "trackCaret", "1" }, { "trackFocus", "0" }, { "mouseAlign", "0" }, { "trackAlign", "0" },
    };
    TrayStatus st;
    st.level = 7.4;
    float ticks[TickStats::kCap];
    for (float& t : ticks) t = 6.94f;
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
