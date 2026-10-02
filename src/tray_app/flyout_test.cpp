// `WindTray.exe --render-test out.png [--light] [--dpi N] [--hover kind[:i]]` (issue #313): renders
// the flyout with fake status to a PNG and exits, so the drawing can be compared with the j01
// references in docs/design/tray-2026-10 without a desktop session or a running Wind.
//   --light            the light theme (default dark)
//   --dpi N            scale (96 = 1x, 192 = 2x like the reference PNGs)
//   --hover K[:I]      hover state: chip:1, settings, quit, profile
//   --tools            adds the #315 chips: engine, Mouse lock, Pass keys, Pause (a second chip row)
//   --listen lock|pass that listen chip pulses (a still frame, at 0.6)
//   --paused           the Pause chip is ON
//   --category N       the window kind in front for the engine chip (0 game, 1 acrylic, 2 desktop, 3 other)
//   --model M          the main engine (hybrid = Auto; render/transform/magnify disable the engine chip)
//   --engine-list      renders the engine chip's open list instead of the flyout (the picked value from --pref)
//   --pref N           the engine row's value in the list: 0 Auto, 1 Transform, 2 Render
// Fake data: zoom 7.4x at 144 fps, Warmth 40%, Brightness 72%, all three toggles shown (text cursor
// on, focus off, keep-within-edges on), profile "Default", Performance on.
#include "tray_app.h"
#include "flyout_draw.h"
#include "flyout_model.h"
#include "../logging.h"
#include "../resource.h"
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
    bool tools = false, paused = false, engineList = false;
    int listen = 0, category = 0, pref = 0;
    std::string model = "hybrid";
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--render-test" && i + 1 < argc) out = argv[++i];
        else if (a == L"--light") light = true;
        else if (a == L"--tools") tools = true;
        else if (a == L"--paused") paused = true;
        else if (a == L"--engine-list") engineList = true;
        else if (a == L"--listen" && i + 1 < argc) listen = std::wstring(argv[++i]) == L"pass" ? 2 : 1;
        else if (a == L"--category" && i + 1 < argc) category = _wtoi(argv[++i]);
        else if (a == L"--pref" && i + 1 < argc) pref = _wtoi(argv[++i]);
        else if (a == L"--model" && i + 1 < argc) { model.clear(); for (const wchar_t* c = argv[++i]; *c; ++c) model.push_back((char)*c); }
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

    IniValues ini = {
        { "trayPerf", "1" },
        { "colorWarmPct", "40" }, { "colorDimPct", "72" },
        { "trayToggles", tools ? "trackCaret,keepEdges,engine,fixLock,fixPass,pause" : "trackCaret,trackFocus,keepEdges" },
        { "trackCaret", "1" }, { "trackFocus", "0" }, { "mouseAlign", "1" }, { "trackAlign", "1" },
        { "model", model },
    };
    ini[Flyout::EngineKeyFor(category)] = Flyout::EnginePrefValue(pref);
    TrayStatus st;
    st.level = 7.4;
    float ticks[TickStats::kCap];
    // A steady 6.94 ms history (the fps and "6.9 ms" readouts come from it).
    for (int i = 0; i < TickStats::kCap; ++i) ticks[i] = 6.94f;
    Flyout::ToolsInput ti;
    ti.fgCategory = category; ti.paused = paused; ti.listening = listen;
    Flyout::View v = Flyout::BuildView(ini, ParseTrayLayout(ini), st, ticks, TickStats::kCap, L"Default", !light, ti);
    if (engineList) {   // the engine chip's open list, built exactly as the live flyout builds it
        Flyout::ListView lv;
        lv.dark = !light;
        lv.caption = v.engine.caption;
        int widest = 0;
        if (v.engine.enabled) {
            for (int i = 0; i < 3; ++i) {
                lv.names.push_back(Flyout::EnginePrefLabel(i));
                widest = (std::max)(widest, Flyout::MeasureProfileText(lv.names.back()));
            }
            lv.active = v.engine.pref;
        }
        const Flyout::ListGeometry lg = Flyout::ComputeList((int)lv.names.size(), widest,
                                                            (std::max)(1, Flyout::MeasureProfileText(lv.caption)));
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

int RunIconTest(const wchar_t*) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return 2;
    std::wstring path;
    for (int i = 1; i + 1 < argc; ++i) if (std::wstring(argv[i]) == L"--icon-test") path = argv[i + 1];
    LocalFree(argv);
    if (path.empty()) return 2;
    struct St { bool pause, dot; float amt; };
    const St states[] = { { false, false, 0.f }, { true, false, 0.f }, { false, true, 0.35f },
                          { false, true, 1.f }, { true, true, 1.f } };
    const int n = 5, kScale = 8, kCell = 32 * kScale + 16, kRowH2 = 32 * kScale + 8;
    const int W = n * kCell, H = 4 * kRowH2;
    HDC sdc = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(sdc);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 24;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    SelectObject(dc, dib);
    int row = 0;
    for (int size : { 16, 32 }) {
        for (int light = 0; light < 2; ++light, ++row) {
            const COLORREF bg = light ? RGB(240, 240, 240) : RGB(32, 32, 32);
            RECT strip{ 0, row * kRowH2, W, (row + 1) * kRowH2 };
            HBRUSH br = CreateSolidBrush(bg);
            FillRect(dc, &strip, br);
            DeleteObject(br);
            HICON base = (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_WIND), IMAGE_ICON, size, size, LR_DEFAULTCOLOR);
            for (int k = 0; k < n; ++k) {
                HICON ic = (states[k].pause || states[k].dot) ? BadgedIconForTest(base, states[k].pause, states[k].dot, states[k].amt) : base;
                if (!ic) ic = base;
                HDC sm = CreateCompatibleDC(sdc);
                HBITMAP sb = CreateCompatibleBitmap(sdc, size, size);
                SelectObject(sm, sb);
                RECT r{ 0, 0, size, size };
                HBRUSH bb = CreateSolidBrush(bg);
                FillRect(sm, &r, bb);
                DeleteObject(bb);
                DrawIconEx(sm, 0, 0, ic, size, size, 0, nullptr, DI_NORMAL);
                SetStretchBltMode(dc, COLORONCOLOR);
                StretchBlt(dc, k * kCell + 8, row * kRowH2 + 4, size * kScale, size * kScale, sm, 0, 0, size, size, SRCCOPY);
                DeleteObject(sb);
                DeleteDC(sm);
            }
        }
    }
    const int stride = (W * 3 + 3) & ~3;
    BITMAPFILEHEADER fh{};
    fh.bfType = 0x4D42; fh.bfOffBits = sizeof(fh) + sizeof(BITMAPINFOHEADER); fh.bfSize = fh.bfOffBits + stride * H;
    BITMAPINFOHEADER ih = bi.bmiHeader;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return 1;
    DWORD wr = 0;
    WriteFile(f, &fh, sizeof(fh), &wr, nullptr);
    WriteFile(f, &ih, sizeof(ih), &wr, nullptr);
    WriteFile(f, bits, stride * H, &wr, nullptr);
    CloseHandle(f);
    return 0;
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
