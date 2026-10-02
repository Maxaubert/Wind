// `WindTray.exe --render-test out.png [--light] [--palette ID] [--dpi N] [--hover kind[:i]]` (issue #313): renders
// the flyout with fake status to a PNG and exits, so the drawing can be compared with the j01
// references in docs/design/tray-2026-10 without a desktop session or a running Wind.
//   --light            the light theme (default dark)
//   --palette ID       the theme: grey (default) ember cyber mono slate carbon hicon ocean; unknown = grey, like the live flyout
//   --dpi N            scale (96 = 1x, 192 = 2x like the reference PNGs)
//   --hover K[:I]      hover state: toggle:1, engine, settings, quit, profile
//   --tools            adds the #315 engine dropdown under the toggle group
//   --toggles a,b,c    the enabled toggle keys, in order (trackCaret, trackFocus, keepEdges, engine)
//   --model M          the main engine shown on the dropdown: hybrid (Auto), render, transform
//   --engine-open      the dropdown drawn open (chevron up, ring)
//   --engine-list      renders the engine list popup instead of the flyout (the active row is --model)
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
    int palette = 0;
    Flyout::Hit hover;
    bool tools = false, engineList = false, engineOpen = false;
    std::string toggles, model = "hybrid";
    auto narrow = [](const wchar_t* c) { std::string o; for (; *c; ++c) o.push_back((char)*c); return o; };
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--render-test" && i + 1 < argc) out = argv[++i];
        else if (a == L"--light") light = true;
        else if (a == L"--palette" && i + 1 < argc) palette = Flyout::PaletteIndex(narrow(argv[++i]));
        else if (a == L"--tools") tools = true;
        else if (a == L"--engine-list") engineList = true;
        else if (a == L"--engine-open") engineOpen = true;
        else if (a == L"--toggles" && i + 1 < argc) toggles = narrow(argv[++i]);
        else if (a == L"--model" && i + 1 < argc) model = narrow(argv[++i]);
        else if (a == L"--dpi" && i + 1 < argc) dpi = (std::max)(96, _wtoi(argv[++i]));
        else if (a == L"--hover" && i + 1 < argc) {
            const std::wstring k = argv[++i];
            if (k.rfind(L"toggle", 0) == 0) hover = { Flyout::HitKind::Toggle, k.size() > 7 ? _wtoi(k.c_str() + 7) : 0 };
            else if (k == L"engine") hover = { Flyout::HitKind::Engine, 0 };
            else if (k == L"settings") hover = { Flyout::HitKind::Settings, 0 };
            else if (k == L"quit") hover = { Flyout::HitKind::Quit, 0 };
            else if (k == L"profile") hover = { Flyout::HitKind::Profile, 0 };
        }
    }
    LocalFree(argv);
    if (out.empty() || !Flyout::DrawInit()) return 2;

    IniValues ini = {
        { "trayPerf", "1" },
        { "colorWarmPct", "40" }, { "colorDimPct", "72" },
        { "trayToggles", !toggles.empty() ? toggles : (tools ? "trackCaret,trackFocus,keepEdges,engine" : "trackCaret,trackFocus,keepEdges") },
        { "trackCaret", "1" }, { "trackFocus", "0" }, { "mouseAlign", "1" }, { "trackAlign", "1" },
        { "model", model },
    };
    TrayStatus st;
    st.level = 7.4;
    float ticks[TickStats::kCap];
    // A steady 6.94 ms history (the fps and "6.9 ms" readouts come from it).
    for (int i = 0; i < TickStats::kCap; ++i) ticks[i] = 6.94f;
    Flyout::View v = Flyout::BuildView(ini, ParseTrayLayout(ini), st, ticks, TickStats::kCap, L"Default", !light, palette);
    if (v.hasEngine) v.engine.open = engineOpen;
    if (engineList) {   // the engine dropdown's open list, built exactly as the live flyout builds it
        Flyout::ListView lv;
        lv.dark = !light;
        lv.palette = palette;
        int widest = 0;
        for (int i = 0; i < Flyout::kEngineCount; ++i) {
            lv.names.push_back(Flyout::EngineLabel(i));
            widest = (std::max)(widest, Flyout::MeasureProfileText(lv.names.back()));
        }
        lv.active = Flyout::EngineIndex(model);
        const Flyout::ListGeometry lg = Flyout::ComputeList(Flyout::kEngineCount, widest, 0, Flyout::ComputeGeometry(v, 0).engine.w());
        return Flyout::RenderListToPng(lv, lg, dpi, out.c_str()) ? 0 : 1;
    }
    v.hover = hover;
    {   // The j01 trace is a gentle low-amplitude wobble, not stalls: the same 17 polyline points the
        // mockup draws (viewBox 0 0 100 14), resampled to 101 evenly spaced samples and mapped to the
        // painter's 0 (bottom) .. 1 (top) scale for a 14 px high line box.
        static const float px[] = { 0, 6, 12, 18, 24, 30, 36, 42, 48, 54, 60, 66, 72, 78, 84, 90, 100 };
        static const float py[] = { 8, 7, 9, 6, 8, 7, 9, 5, 8, 7, 9, 6, 8, 7, 9, 6, 8 };
        v.p.spark.clear();
        for (int i = 0; i <= 100; ++i) {
            int k = 0;
            while (k < 15 && (float)i > px[k + 1]) ++k;
            const float t = ((float)i - px[k]) / (px[k + 1] - px[k]);
            const float y = py[k] + (py[k + 1] - py[k]) * t;
            v.p.spark.push_back((13.f - y) / 12.f);
        }
    }
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
