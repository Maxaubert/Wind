// The tray flyout window (WindTray.exe, issue #313): a layered popup (true per-pixel alpha, exact
// rounded corners) painted by flyout_draw.cpp, placed by flyout_model.h next to the tray icon.
// It lives on the main thread: there is no modal loop any more, so Wind's tick, the tray icon and
// the live header all run on ordinary messages.
//
// Dismissal: deactivation (a click anywhere else, alt-tab, Win key), Esc, and a second click on the
// tray icon all close it exactly once. The icon click DEACTIVATES us first and then arrives as a
// click of its own, which would reopen the flyout: a close by deactivation within kReopenGuardMs
// of that click is that click, so ToggleFlyout ignores it (IgnoreIconClick, unit-tested).
#include "tray_app.h"
#include "flyout_draw.h"
#include "flyout_model.h"
#include "../logging.h"
#include "../config_path.h"
#include "../profiles_io.h"
#include "../config.h"
#include "../config_ui/ini_edit.h"
#include "../tray_ipc.h"
#include <shellscalingapi.h>
#include <string>
#pragma comment(lib, "shcore.lib")

namespace wind { namespace TrayApp {

namespace {

const wchar_t* const kClass = L"WindTrayFlyout";
const UINT_PTR kTimerId = 1;
const UINT kTimerMs = 100;                       // ~10 Hz while open (performance + outside-click fallback)
const UINT WM_FLYOUT_CLOSE = WM_APP + 20;

struct State {
    HWND hwnd = nullptr;
    int dpi = 96, pw = 0, ph = 0;
    POINT pos{};
    std::wstring iniPath;
    IniValues ini;
    TrayLayout layout;
    bool dark = true;
    std::wstring profile;
    Flyout::Geometry geo;
    Flyout::View view;
    HDC dc = nullptr;
    HBITMAP dib = nullptr, oldBmp = nullptr;
    ID2D1DCRenderTarget* rt = nullptr;
    Flyout::Painter* painter = nullptr;
    bool activated = false, closing = false, leaveTracked = false;
    Flyout::Hit down;
};

State* g_f = nullptr;
ULONGLONG g_deactivatedAt = 0;
ATOM g_cls = 0;

void RebuildView(State& s) {
    TrayShared* blk = Block();
    const TrayStatus st = ReadTrayStatus(blk);
    float buf[TickStats::kCap];
    int n = 0;
    if (TrayBlockValid(blk)) n = blk->ticks.snapshot(buf, TickStats::kCap);
    const Flyout::Hit hover = s.view.hover;
    s.view = Flyout::BuildView(s.ini, s.layout, st, buf, n, s.profile, s.dark);
    s.view.hover = hover;
}

void Render(State& s) {
    if (!s.rt || !s.painter || !s.dc) return;
    RECT rc{ 0, 0, s.pw, s.ph };
    if (FAILED(s.rt->BindDC(s.dc, &rc))) return;
    s.rt->BeginDraw();
    s.painter->Draw(s.view, s.geo);
    const HRESULT hr = s.rt->EndDraw();
    if (FAILED(hr)) {
        wind::Log(wind::LogLevel::Warn, "tray", "flyout draw failed (hr=0x%08lx)", (unsigned long)hr);
        return;
    }
    POINT src{ 0, 0 }, dst = s.pos;
    SIZE sz{ s.pw, s.ph };
    BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(s.hwnd, nullptr, &dst, &sz, s.dc, &src, 0, &bf, ULW_ALPHA);
}

Flyout::Hit HitAt(const State& s, LPARAM l) {
    const int x = Flyout::ToDip((short)LOWORD(l), s.dpi), y = Flyout::ToDip((short)HIWORD(l), s.dpi);
    return Flyout::HitTest(s.geo, x, y);
}

LRESULT CALLBACK FlyoutProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    State* s = g_f;
    if (!s || s->hwnd != h) return DefWindowProcW(h, m, w, l);
    switch (m) {
        case WM_MOUSEACTIVATE: return MA_ACTIVATE;
        case WM_ERASEBKGND: return 1;
        case WM_ACTIVATE:
            if (LOWORD(w) == WA_INACTIVE) {
                if (!s->closing) {
                    g_deactivatedAt = GetTickCount64();
                    PostMessageW(h, WM_FLYOUT_CLOSE, 0, 0);
                }
            } else {
                s->activated = true;
            }
            return 0;
        case WM_FLYOUT_CLOSE: CloseFlyout(); return 0;
        case WM_KEYDOWN:
            if (w == VK_ESCAPE) { CloseFlyout(); return 0; }
            break;
        case WM_MOUSEMOVE: {
            if (!s->leaveTracked) {
                TRACKMOUSEEVENT t{ sizeof(t), TME_LEAVE, h, 0 };
                s->leaveTracked = TrackMouseEvent(&t) != FALSE;
            }
            const Flyout::Hit hit = HitAt(*s, l);
            if (hit != s->view.hover) { s->view.hover = hit; Render(*s); }
            return 0;
        }
        case WM_MOUSELEAVE:
            s->leaveTracked = false;
            if (s->view.hover.kind != Flyout::HitKind::None) { s->view.hover = {}; Render(*s); }
            return 0;
        case WM_LBUTTONDOWN:
            s->down = HitAt(*s, l);
            return 0;
        case WM_LBUTTONUP: {
            const Flyout::Hit hit = HitAt(*s, l);
            const Flyout::Hit down = s->down;
            s->down = {};
            if (hit != down) return 0;
            const std::wstring ini = s->iniPath;
            if (hit.kind == Flyout::HitKind::Settings) {
                CloseFlyout();
                OpenSettings();
            } else if (hit.kind == Flyout::HitKind::Quit) {
                CloseFlyout();                                  // the prompt must not sit under us
                if (ConfirmQuit(ini)) RequestWindQuit();
            }
            return 0;
        }
        case WM_TIMER:
            if (w == kTimerId) {
                if (!s->activated && ((GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_RBUTTON)) & 0x8000)) {
                    // We could not take foreground, so no deactivation will ever come: a click
                    // outside is the dismissal.
                    POINT p; RECT wr;
                    if (GetCursorPos(&p) && GetWindowRect(h, &wr) && !PtInRect(&wr, p)) { CloseFlyout(); return 0; }
                }
                if (s->view.perf) { RebuildView(*s); Render(*s); }
            }
            return 0;
        case WM_DESTROY: {
            KillTimer(h, kTimerId);
            SetTrayMenuOpen(Block(), false);
            delete s->painter;
            if (s->rt) s->rt->Release();
            if (s->dc) { if (s->oldBmp) SelectObject(s->dc, s->oldBmp); DeleteDC(s->dc); }
            if (s->dib) DeleteObject(s->dib);
            g_f = nullptr;
            delete s;
            return 0;
        }
    }
    return DefWindowProcW(h, m, w, l);
}

bool OpenFlyout() {
    if (!Flyout::DrawInit()) {
        wind::Log(wind::LogLevel::Error, "tray", "flyout: Direct2D init failed");
        return false;
    }
    auto* s = new State;
    s->iniPath = wind::ResolveIniPath();
    const std::string text = wind::ReadTextFile(s->iniPath);
    s->ini = wind::ReadIniValues(text);
    s->layout = ParseTrayLayout(s->ini);
    s->dark = UsesDarkTheme(text);
    auto pit = s->ini.find("profile");
    s->profile = pit == s->ini.end() ? std::wstring() : wind::WidenUtf8(pit->second);

    // Anchor: the icon, or the cursor when the shell cannot say (icon in the overflow flyout).
    RECT icon{};
    if (!GetIconRect(&icon)) {
        POINT p; GetCursorPos(&p);
        icon = { p.x, p.y, p.x + 1, p.y + 1 };
    }
    HMONITOR mon = MonitorFromRect(&icon, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(mon, &mi);
    UINT dx = 96, dy = 96;
    if (FAILED(GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy)) || dx == 0) dx = 96;
    s->dpi = (int)dx;

    RebuildView(*s);
    s->geo = Flyout::ComputeGeometry(s->view.perf, (int)s->view.sliders.size(), (int)s->view.toggles.size(),
                                     Flyout::MeasureProfileText(s->view.profile));
    s->pw = Flyout::ScalePx(s->geo.width, s->dpi);
    s->ph = Flyout::ScalePx(s->geo.height, s->dpi);
    const Flyout::IRect ir{ icon.left, icon.top, icon.right, icon.bottom };
    const Flyout::IRect mr{ mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right, mi.rcMonitor.bottom };
    const Flyout::IRect wr{ mi.rcWork.left, mi.rcWork.top, mi.rcWork.right, mi.rcWork.bottom };
    const Flyout::Placement pl = Flyout::PlaceFlyout(ir, mr, wr, s->pw, s->ph, Flyout::ScalePx(8, s->dpi));
    s->pos = { pl.x, pl.y };

    if (!g_cls) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = FlyoutProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kClass;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        g_cls = RegisterClassW(&wc);
    }
    g_f = s;
    s->hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kClass, L"Wind", WS_POPUP,
                              s->pos.x, s->pos.y, s->pw, s->ph, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!s->hwnd) {
        wind::Log(wind::LogLevel::Error, "tray", "flyout window create failed (err=%lu)", GetLastError());
        g_f = nullptr;
        delete s;
        return false;
    }

    // Per-pixel-alpha surface: a premultiplied top-down DIB that Direct2D draws into through a DC
    // target, then UpdateLayeredWindow publishes.
    HDC screen = GetDC(nullptr);
    s->dc = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = s->pw;
    bi.bmiHeader.biHeight = -s->ph;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    s->dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    s->rt = s->dib ? Flyout::CreateDcTarget(s->dpi) : nullptr;
    s->painter = new Flyout::Painter;
    if (!s->dib || !s->rt || !s->painter->Init(s->rt, s->dark)) {
        wind::Log(wind::LogLevel::Error, "tray", "flyout surface init failed");
        DestroyWindow(s->hwnd);
        return false;
    }
    s->oldBmp = static_cast<HBITMAP>(SelectObject(s->dc, s->dib));
    Render(*s);

    SetTrayMenuOpen(Block(), true);     // Wind suspends the cursor re-park while the flyout is open
    SetWindowPos(s->hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetForegroundWindow(s->hwnd);       // we own the last input (the tray click), so this is permitted
    SetTimer(s->hwnd, kTimerId, kTimerMs, nullptr);
    return true;
}

}  // namespace

bool FlyoutIsOpen() { return g_f != nullptr; }

void CloseFlyout() {
    if (!g_f) return;
    g_f->closing = true;
    DestroyWindow(g_f->hwnd);
}

void ToggleFlyout() {
    if (g_f) { CloseFlyout(); return; }     // a second click while it is still open and active
    if (Flyout::IgnoreIconClick(GetTickCount64(), g_deactivatedAt)) return;
    OpenFlyout();
}

}}  // namespace wind::TrayApp
