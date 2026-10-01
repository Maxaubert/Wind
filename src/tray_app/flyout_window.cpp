// The tray flyout window (WindTray.exe, issue #313): a layered popup (true per-pixel alpha, exact
// rounded corners) painted by flyout_draw.cpp, placed by flyout_model.h next to the tray icon.
// It lives on the main thread: there is no modal loop any more, so Wind's tick, the tray icon and
// the live header all run on ordinary messages.
//
// Dismissal: deactivation (a click anywhere else, alt-tab, Win key), Esc, and a second click on the
// tray icon all close it exactly once. The icon click DEACTIVATES us first and then arrives as a
// click of its own (on button UP), which would reopen the flyout: a close by deactivation while the
// button is held on the icon (any hold length), or within kReopenGuardMs, is that click, so
// ToggleFlyout ignores it once (IgnoreIconClick, unit-tested).
//
// Interaction: sliders drag with the mouse (capture) or the arrow keys and write the live ini
// through the same helper the config host uses, throttled to one write per ~50 ms with the final
// value always landing (WriteThrottle + a flush timer + a flush on release and on close). Chips
// toggle. Tab / Shift+Tab walk the controls. The profile button opens a small list window (its own
// non-activating layered popup owned by the flyout, so the flyout stays active and every dismissal
// above still holds). Quit and a profile switch go through the unsaved-settings prompt.
#include "tray_app.h"
#include "flyout_draw.h"
#include "flyout_model.h"
#include "../logging.h"
#include "../config_path.h"
#include "../profiles_io.h"
#include "../config.h"
#include "../config_ui/ini_edit.h"
#include "../tray_ipc.h"
#include <commctrl.h>
#include <shellscalingapi.h>
#include <uxtheme.h>
#include <cmath>
#include <map>
#include <string>
#pragma comment(lib, "shcore.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "uxtheme.lib")

namespace wind { namespace TrayApp {

namespace {

const wchar_t* const kClass = L"WindTrayFlyout";
const wchar_t* const kListClass = L"WindTrayFlyoutList";
const UINT_PTR kTimerId = 1;
const UINT_PTR kFlushTimerId = 2;
const UINT kTimerMs = 100;                       // ~10 Hz while open (performance + outside-click fallback)
const UINT WM_FLYOUT_CLOSE = WM_APP + 20;
const int kMaxFlushRetries = 20;                 // a locked ini is retried for about a second, then dropped

// A layered window's per-pixel-alpha surface: a premultiplied top-down DIB that Direct2D draws
// into through a DC target, then UpdateLayeredWindow publishes.
struct Surface {
    HWND hwnd = nullptr;
    int pw = 0, ph = 0, dpi = 96;
    void* bits = nullptr;
    POINT pos{};
    HDC dc = nullptr;
    HBITMAP dib = nullptr, oldBmp = nullptr;
    ID2D1DCRenderTarget* rt = nullptr;
    Flyout::Painter* painter = nullptr;

    bool Init(HWND h, int w, int ht, int dpiArg, bool dark) {
        hwnd = h; pw = w; ph = ht; dpi = dpiArg;
        HDC screen = GetDC(nullptr);
        dc = CreateCompatibleDC(screen);
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = pw;
        bi.bmiHeader.biHeight = -ph;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        ReleaseDC(nullptr, screen);
        rt = dib ? Flyout::CreateDcTarget(dpiArg) : nullptr;
        painter = new Flyout::Painter;
        if (!dc || !dib || !rt || !painter->Init(rt, dark)) return false;
        oldBmp = static_cast<HBITMAP>(SelectObject(dc, dib));
        return true;
    }
    void Release() {
        delete painter; painter = nullptr;
        if (rt) { rt->Release(); rt = nullptr; }
        if (dc) { if (oldBmp) SelectObject(dc, oldBmp); DeleteDC(dc); dc = nullptr; }
        if (dib) { DeleteObject(dib); dib = nullptr; }
    }
    template <class F> void Present(F draw) {
        if (!rt || !painter || !dc) return;
        RECT rc{ 0, 0, pw, ph };
        if (FAILED(rt->BindDC(dc, &rc))) return;
        rt->BeginDraw();
        draw(*painter);
        const HRESULT hr = rt->EndDraw();
        if (FAILED(hr)) {
            wind::Log(wind::LogLevel::Warn, "tray", "flyout draw failed (hr=0x%08lx)", (unsigned long)hr);
            return;
        }
        if (bits) Flyout::ApplyShapeAlpha(static_cast<unsigned char*>(bits), pw, ph, pw * 4, dpi, true);
        POINT src{ 0, 0 }, dst = pos;
        SIZE sz{ pw, ph };
        BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
        UpdateLayeredWindow(hwnd, nullptr, &dst, &sz, dc, &src, 0, &bf, ULW_ALPHA);
    }
};

struct State {
    Surface sf;
    HWND hwnd = nullptr, tip = nullptr;
    int dpi = 96;
    std::wstring iniPath;
    IniValues ini;
    TrayLayout layout;
    bool dark = true;
    std::wstring profile;
    Flyout::IRect work;                          // work area of the icon's monitor, pixels
    Flyout::Geometry geo;
    Flyout::View view;
    bool activated = false, closing = false, leaveTracked = false;
    Flyout::Hit down;
    int drag = -1;                               // slider being dragged, or -1
    std::map<std::string, std::string> pending;  // ini writes waiting for the throttle
    Flyout::WriteThrottle thr;
    bool flushArmed = false;
    int flushRetries = 0;
};

struct ListState {
    Surface sf;
    Flyout::ListGeometry geo;
    Flyout::ListView view;
    bool leaveTracked = false;
};

State* g_f = nullptr;
ListState* g_list = nullptr;
ULONGLONG g_deactivatedAt = 0;
bool g_pressHeldOnIcon = false;   // a mouse button was down over the tray icon at deactivation
ATOM g_cls = 0, g_listCls = 0;

bool PressHeldOnIcon() {
    if (!((GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_RBUTTON)) & 0x8000)) return false;
    RECT r; POINT p;
    if (!GetIconRect(&r) || !GetCursorPos(&p)) return false;
    InflateRect(&r, 2, 2);
    return PtInRect(&r, p) != FALSE;
}

void RebuildView(State& s) {
    TrayShared* blk = Block();
    const TrayStatus st = ReadTrayStatus(blk);
    float buf[TickStats::kCap];
    int n = 0;
    if (TrayBlockValid(blk)) n = blk->ticks.snapshot(buf, TickStats::kCap);
    const Flyout::Hit hover = s.view.hover, focus = s.view.focus;
    const bool showFocus = s.view.showFocus;
    s.view = Flyout::BuildView(s.ini, s.layout, st, buf, n, s.profile, s.dark);
    s.view.hover = hover;
    s.view.focus = focus;
    s.view.showFocus = showFocus;
}

void Render(State& s) {
    s.sf.Present([&](Flyout::Painter& p) { p.Draw(s.view, s.geo); });
}

void RenderList(ListState& l) {
    l.sf.Present([&](Flyout::Painter& p) { p.DrawList(l.view, l.geo); });
}

Flyout::Hit HitAt(const State& s, LPARAM l) {
    const int x = Flyout::ToDip((short)LOWORD(l), s.dpi), y = Flyout::ToDip((short)HIWORD(l), s.dpi);
    return Flyout::HitTest(s.geo, x, y);
}

// ---------------------------------------------------------------- applying changes

// Writes the pending keys into the live ini (read-modify-write, atomic), so a hand edit or a
// Settings write made meanwhile is kept. A locked ini keeps the changes pending and retries.
void FlushPending(State& s) {
    if (s.flushArmed) { KillTimer(s.hwnd, kFlushTimerId); s.flushArmed = false; }
    if (s.pending.empty()) return;
    std::string text;
    if (GetFileAttributesW(s.iniPath.c_str()) != INVALID_FILE_ATTRIBUTES &&
        !wind::ReadTextFileOk(s.iniPath, text)) {
        if (++s.flushRetries <= kMaxFlushRetries && !s.closing) {
            SetTimer(s.hwnd, kFlushTimerId, Flyout::WriteThrottle::kMinMs, nullptr);
            s.flushArmed = true;
        } else {
            wind::Log(wind::LogLevel::Warn, "tray", "flyout: ini unreadable, %zu change(s) dropped", s.pending.size());
            s.pending.clear();
        }
        return;
    }
    for (const auto& kv : s.pending) text = wind::UpdateIniText(text, kv.first, kv.second);
    if (!wind::WriteTextFileAtomic(s.iniPath, text))
        wind::Log(wind::LogLevel::Warn, "tray", "flyout: ini write failed (err=%lu)", GetLastError());
    s.pending.clear();
    s.flushRetries = 0;
    s.thr.wrote(GetTickCount64());
}

// Applies changes to the in-memory ini and the view at once; the file write is immediate when
// `final` (a click, a release) or when the throttle allows, else it waits for the flush timer.
void Commit(State& s, const std::vector<Flyout::IniChange>& ch, bool final) {
    Flyout::ApplyChanges(s.ini, ch);
    for (const auto& c : ch) s.pending[c.key] = c.value;
    RebuildView(s);
    Render(s);
    const ULONGLONG now = GetTickCount64();
    if (final || s.thr.due(now)) { FlushPending(s); return; }
    if (!s.flushArmed) {
        SetTimer(s.hwnd, kFlushTimerId, (UINT)(std::max)(1ULL, s.thr.waitMs(now)), nullptr);
        s.flushArmed = true;
    }
}

const Flyout::SliderSpec* SpecAt(const State& s, int i) {
    return i >= 0 && i < (int)s.view.sliders.size() ? Flyout::FindSliderSpec(s.view.sliders[i].key) : nullptr;
}

void SetSliderValue(State& s, int i, double v, bool final) {
    const Flyout::SliderSpec* sp = SpecAt(s, i);
    if (!sp) return;
    v = Flyout::SnapSlider(*sp, v);
    if (std::fabs(v - Flyout::SliderValue(*sp, s.ini)) < 1e-9) {
        if (final) FlushPending(s);
        return;
    }
    Commit(s, { { sp->key, Flyout::FormatIniValue(*sp, v) } }, final);
}

void DragTo(State& s, LPARAM l, bool final) {
    const Flyout::SliderSpec* sp = SpecAt(s, s.drag);
    if (!sp) return;
    const int x = Flyout::ToDip((short)LOWORD(l), s.dpi);
    SetSliderValue(s, s.drag, Flyout::SliderFromX(*sp, s.geo.sliderTrack[s.drag], x), final);
}

void ToggleChip(State& s, int i) {
    if (i < 0 || i >= (int)s.view.toggles.size()) return;
    const Flyout::ToggleView& t = s.view.toggles[i];
    Commit(s, Flyout::ToggleChanges(t.key, !t.on), true);
}

// ---------------------------------------------------------------- profile list popup

void CloseList() {
    if (!g_list) return;
    DestroyWindow(g_list->sf.hwnd);       // WM_DESTROY frees the state
}

void ChooseProfile(int idx) {
    if (!g_list || !g_f || idx < 0 || idx >= (int)g_list->view.names.size()) return;
    const std::wstring name = g_list->view.names[idx];
    const bool same = idx == g_list->view.active;
    const std::wstring ini = g_f->iniPath;
    CloseFlyout();                        // the prompt must not sit under us (and closes the list)
    if (same) return;
    if (ConfirmSwitch(ini)) SwitchToProfile(ini, name);
}

LRESULT CALLBACK ListProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    ListState* ls = g_list;
    if (!ls || ls->sf.hwnd != h) return DefWindowProcW(h, m, w, l);
    switch (m) {
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;       // the flyout stays the active window
        case WM_ERASEBKGND: return 1;
        case WM_MOUSEMOVE: {
            if (!ls->leaveTracked) {
                TRACKMOUSEEVENT t{ sizeof(t), TME_LEAVE, h, 0 };
                ls->leaveTracked = TrackMouseEvent(&t) != FALSE;
            }
            const int dpi = g_f ? g_f->dpi : 96;
            const int i = Flyout::ListHitTest(ls->geo, Flyout::ToDip((short)LOWORD(l), dpi),
                                              Flyout::ToDip((short)HIWORD(l), dpi));
            if (i != ls->view.sel) { ls->view.sel = i; RenderList(*ls); }
            return 0;
        }
        case WM_MOUSELEAVE:
            ls->leaveTracked = false;
            if (ls->view.sel != -1) { ls->view.sel = -1; RenderList(*ls); }
            return 0;
        case WM_LBUTTONUP: {
            const int dpi = g_f ? g_f->dpi : 96;
            const int i = Flyout::ListHitTest(ls->geo, Flyout::ToDip((short)LOWORD(l), dpi),
                                              Flyout::ToDip((short)HIWORD(l), dpi));
            if (i >= 0) ChooseProfile(i);
            return 0;
        }
        case WM_DESTROY:
            ls->sf.Release();
            g_list = nullptr;
            delete ls;
            return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

void OpenList(State& s, bool keyboard) {
    if (g_list) return;
    std::vector<std::wstring> names = wind::ListProfileFiles(wind::ProfilesDirFromIni(s.iniPath));
    if (names.empty()) return;
    auto* ls = new ListState;
    ls->view.dark = s.dark;
    int widest = 0;
    for (size_t i = 0; i < names.size(); ++i) {
        widest = (std::max)(widest, Flyout::MeasureProfileText(names[i]));
        if (_wcsicmp(names[i].c_str(), s.profile.c_str()) == 0) ls->view.active = (int)i;
    }
    ls->view.names = names;
    ls->view.sel = keyboard ? ls->view.active : -1;
    ls->geo = Flyout::ComputeList((int)names.size(), widest);
    ls->sf.pw = Flyout::ScalePx(ls->geo.width, s.dpi);
    ls->sf.ph = Flyout::ScalePx(ls->geo.height, s.dpi);
    const Flyout::IRect& b = s.geo.profileBtn;
    const Flyout::IRect anchor{ s.sf.pos.x + Flyout::ScalePx(b.l, s.dpi), s.sf.pos.y + Flyout::ScalePx(b.t, s.dpi),
                                s.sf.pos.x + Flyout::ScalePx(b.r, s.dpi), s.sf.pos.y + Flyout::ScalePx(b.b, s.dpi) };
    const Flyout::Placement pl = Flyout::PlaceList(anchor, s.work, ls->sf.pw, ls->sf.ph, Flyout::ScalePx(4, s.dpi));
    ls->sf.pos = { pl.x, pl.y };

    if (!g_listCls) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = ListProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kListClass;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        g_listCls = RegisterClassW(&wc);
    }
    g_list = ls;
    HWND h = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, kListClass, L"Wind profiles",
                             WS_POPUP, ls->sf.pos.x, ls->sf.pos.y, ls->sf.pw, ls->sf.ph, s.hwnd, nullptr,
                             GetModuleHandleW(nullptr), nullptr);
    if (!h) {
        g_list = nullptr;
        delete ls;
        return;
    }
    if (!ls->sf.Init(h, ls->sf.pw, ls->sf.ph, s.dpi, s.dark)) {
        wind::Log(wind::LogLevel::Error, "tray", "flyout: profile list surface init failed");
        DestroyWindow(h);                 // WM_DESTROY frees ls
        return;
    }
    RenderList(*ls);
    SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

// Keys while the list is open: it owns Up/Down/Enter/Space/Esc and swallows the rest.
bool ListKey(WPARAM vk) {
    ListState* ls = g_list;
    if (!ls) return false;
    const int n = (int)ls->view.names.size();
    switch (vk) {
        case VK_UP:   ls->view.sel = Flyout::ListStep(ls->view.sel, n, -1); RenderList(*ls); break;
        case VK_DOWN: ls->view.sel = Flyout::ListStep(ls->view.sel, n, +1); RenderList(*ls); break;
        case VK_HOME: ls->view.sel = 0; RenderList(*ls); break;
        case VK_END:  ls->view.sel = n - 1; RenderList(*ls); break;
        case VK_RETURN: case VK_SPACE: if (ls->view.sel >= 0) ChooseProfile(ls->view.sel); break;
        case VK_ESCAPE: case VK_LEFT: CloseList(); break;
        default: break;
    }
    return true;
}

// ---------------------------------------------------------------- keyboard

void Activate(State& s, const Flyout::Hit& h) {
    switch (h.kind) {
        case Flyout::HitKind::Chip: ToggleChip(s, h.index); break;
        case Flyout::HitKind::Profile: OpenList(s, true); break;
        case Flyout::HitKind::Settings: CloseFlyout(); OpenSettings(); break;
        case Flyout::HitKind::Quit: {
            const std::wstring ini = s.iniPath;
            CloseFlyout();
            if (ConfirmQuit(ini)) RequestWindQuit();
            break;
        }
        default: break;
    }
}

bool OnKey(State& s, WPARAM vk, LPARAM l) {
    if (g_list) return ListKey(vk);
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool repeat = (l & (1 << 30)) != 0;
    const int n = Flyout::FocusCount(s.geo);
    const Flyout::Hit f = s.view.focus;
    const bool focused = s.view.showFocus && f.kind != Flyout::HitKind::None;
    switch (vk) {
        case VK_ESCAPE: CloseFlyout(); return true;
        case VK_TAB: {
            const int cur = Flyout::FocusIndex(s.geo, f);
            s.view.focus = Flyout::FocusHit(s.geo, Flyout::NextFocus(cur, n, shift));
            s.view.showFocus = true;
            Render(s);
            return true;
        }
        case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_HOME: case VK_END: {
            if (!focused) return false;
            const bool inc = vk == VK_RIGHT || vk == VK_UP;
            if (f.kind == Flyout::HitKind::Slider) {
                const Flyout::SliderSpec* sp = SpecAt(s, f.index);
                if (!sp) return true;
                const double cur = Flyout::SliderValue(*sp, s.ini);
                double v = cur;
                if (vk == VK_HOME) v = sp->min;
                else if (vk == VK_END) v = sp->max;
                else v = Flyout::StepSlider(*sp, cur, inc ? +1 : -1, shift);
                SetSliderValue(s, f.index, v, false);
                return true;
            }
            if (f.kind == Flyout::HitKind::Chip && (vk == VK_LEFT || vk == VK_RIGHT)) {
                const int nc = (int)s.geo.chip.size();
                const int to = (std::min)((std::max)(f.index + (inc ? 1 : -1), 0), nc - 1);
                s.view.focus = { Flyout::HitKind::Chip, to };
                Render(s);
                return true;
            }
            return true;
        }
        case VK_SPACE: case VK_RETURN:
            if (focused && !repeat) Activate(s, f);
            return true;
        default: break;
    }
    return false;
}

// ---------------------------------------------------------------- tooltips

void AddTip(State& s, const Flyout::IRect& r, const std::wstring& text, int grow) {
    if (!s.tip || text.empty()) return;
    static UINT_PTR id = 0;
    TOOLINFOW ti{};
    ti.cbSize = sizeof(ti);
    ti.uFlags = TTF_SUBCLASS;
    ti.hwnd = s.hwnd;
    ti.uId = ++id;
    ti.rect = { Flyout::ScalePx(r.l - grow, s.dpi), Flyout::ScalePx(r.t - grow, s.dpi),
                Flyout::ScalePx(r.r + grow, s.dpi), Flyout::ScalePx(r.b + grow, s.dpi) };
    ti.lpszText = const_cast<LPWSTR>(text.c_str());   // copied by the control
    SendMessageW(s.tip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
}

void CreateTips(State& s) {
    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_WIN95_CLASSES };
    InitCommonControlsEx(&icc);
    s.tip = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TRANSPARENT, TOOLTIPS_CLASSW, nullptr,
                            WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, 0, 0, 0, 0, s.hwnd, nullptr,
                            GetModuleHandleW(nullptr), nullptr);
    if (!s.tip) return;
    SetWindowTheme(s.tip, s.dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
    SendMessageW(s.tip, TTM_SETDELAYTIME, TTDT_INITIAL, 500);
    for (size_t i = 0; i < s.view.sliders.size() && i < s.geo.sliderIcon.size(); ++i)
        AddTip(s, s.geo.sliderIcon[i], s.view.sliders[i].name, 6);
    for (size_t i = 0; i < s.view.toggles.size() && i < s.geo.chip.size(); ++i)
        AddTip(s, s.geo.chip[i], s.view.toggles[i].name, 0);
    AddTip(s, s.geo.profileBtn, L"Switch profile", 0);
    AddTip(s, s.geo.settingsBtn, L"Settings", 0);
    AddTip(s, s.geo.quitBtn, L"Quit Wind", 0);
}

// ---------------------------------------------------------------- the window

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
                    g_pressHeldOnIcon = PressHeldOnIcon();
                    PostMessageW(h, WM_FLYOUT_CLOSE, 0, 0);
                }
            } else {
                s->activated = true;
            }
            return 0;
        case WM_FLYOUT_CLOSE: CloseFlyout(); return 0;
        case WM_KEYDOWN:
            if (OnKey(*s, w, l)) return 0;
            break;
        case WM_MOUSEMOVE: {
            if (!s->leaveTracked) {
                TRACKMOUSEEVENT t{ sizeof(t), TME_LEAVE, h, 0 };
                s->leaveTracked = TrackMouseEvent(&t) != FALSE;
            }
            if (s->drag >= 0) { DragTo(*s, l, false); return 0; }
            const Flyout::Hit hit = HitAt(*s, l);
            if (hit != s->view.hover) { s->view.hover = hit; Render(*s); }
            return 0;
        }
        case WM_MOUSELEAVE:
            s->leaveTracked = false;
            if (s->view.hover.kind != Flyout::HitKind::None) { s->view.hover = {}; Render(*s); }
            return 0;
        case WM_LBUTTONDOWN: {
            if (g_list) {                  // a click anywhere on the flyout dismisses the list, and nothing else
                CloseList();
                s->down = {};
                return 0;
            }
            const Flyout::Hit hit = HitAt(*s, l);
            s->down = hit;
            s->view.showFocus = false;
            const int x = Flyout::ToDip((short)LOWORD(l), s->dpi), y = Flyout::ToDip((short)HIWORD(l), s->dpi);
            if (hit.kind == Flyout::HitKind::Slider && Flyout::SliderTrackHit(s->geo, hit.index, x, y)) {
                s->drag = hit.index;
                s->view.focus = hit;
                SetCapture(h);
                DragTo(*s, l, false);
            } else {
                Render(*s);
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            if (s->drag >= 0) {
                DragTo(*s, l, true);       // the final value lands now, whatever the throttle held back
                s->drag = -1;
                s->down = {};
                ReleaseCapture();
                return 0;
            }
            const Flyout::Hit hit = HitAt(*s, l);
            const Flyout::Hit down = s->down;
            s->down = {};
            if (hit != down) return 0;
            if (hit.kind == Flyout::HitKind::Chip) {
                ToggleChip(*s, hit.index);
            } else if (hit.kind == Flyout::HitKind::Profile) {
                OpenList(*s, false);
            } else if (hit.kind == Flyout::HitKind::Settings) {
                CloseFlyout();
                OpenSettings();
            } else if (hit.kind == Flyout::HitKind::Quit) {
                const std::wstring ini = s->iniPath;
                CloseFlyout();             // the prompt must not sit under us
                if (ConfirmQuit(ini)) RequestWindQuit();
            }
            return 0;
        }
        case WM_CAPTURECHANGED:
            if (s->drag >= 0) { s->drag = -1; FlushPending(*s); }
            return 0;
        case WM_TIMER:
            if (w == kFlushTimerId) {
                FlushPending(*s);
            } else if (w == kTimerId) {
                if (!s->activated && ((GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_RBUTTON)) & 0x8000)) {
                    // We could not take foreground, so no deactivation will ever come: a click
                    // outside is the dismissal.
                    POINT p; RECT wr;
                    if (GetCursorPos(&p) && GetWindowRect(h, &wr) && !PtInRect(&wr, p)) {
                        RECT lr;
                        const bool inList = g_list && GetWindowRect(g_list->sf.hwnd, &lr) && PtInRect(&lr, p);
                        if (!inList) { CloseFlyout(); return 0; }
                    }
                }
                if (s->view.perf && s->drag < 0) { RebuildView(*s); Render(*s); }
            }
            return 0;
        case WM_DESTROY: {
            KillTimer(h, kTimerId);
            s->closing = true;
            FlushPending(*s);              // a value still held by the throttle is never lost
            if (s->tip) DestroyWindow(s->tip);
            SetTrayMenuOpen(Block(), false);
            s->sf.Release();
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
    const int pw = Flyout::ScalePx(s->geo.width, s->dpi), ph = Flyout::ScalePx(s->geo.height, s->dpi);
    const Flyout::IRect ir{ icon.left, icon.top, icon.right, icon.bottom };
    const Flyout::IRect mr{ mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right, mi.rcMonitor.bottom };
    s->work = { mi.rcWork.left, mi.rcWork.top, mi.rcWork.right, mi.rcWork.bottom };
    const Flyout::Placement pl = Flyout::PlaceFlyout(ir, mr, s->work, pw, ph, Flyout::ScalePx(8, s->dpi));
    s->sf.pos = { pl.x, pl.y };

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
                              pl.x, pl.y, pw, ph, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!s->hwnd) {
        wind::Log(wind::LogLevel::Error, "tray", "flyout window create failed (err=%lu)", GetLastError());
        g_f = nullptr;
        delete s;
        return false;
    }
    if (!s->sf.Init(s->hwnd, pw, ph, s->dpi, s->dark)) {
        wind::Log(wind::LogLevel::Error, "tray", "flyout surface init failed");
        DestroyWindow(s->hwnd);        // WM_DESTROY releases and frees
        return false;
    }
    CreateTips(*s);
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
    CloseList();
    DestroyWindow(g_f->hwnd);
}

void ToggleFlyout() {
    if (g_f) { CloseFlyout(); return; }     // a second click while it is still open and active
    const bool ignore = Flyout::IgnoreIconClick(GetTickCount64(), g_deactivatedAt, g_pressHeldOnIcon);
    g_deactivatedAt = 0;                    // one deactivation swallows at most one icon click
    g_pressHeldOnIcon = false;
    if (ignore) return;
    OpenFlyout();
}

}}  // namespace wind::TrayApp
