#include "tray.h"
#include "resource.h"
#include "logging.h"
#include "config_path.h"
#include "profiles_io.h"
#include "config.h"
#include "config_ui/ini_edit.h"
#include "tray_draw.h"
#include "tray_status.h"
#include "tick_stats.h"
#include <shellapi.h>
#include <dwmapi.h>
#include <string>
#include <thread>
#include <mutex>
#include <vector>
namespace wind { namespace Tray {
static NOTIFYICONDATAW g_nid{};
static const UINT WM_TRAY = WM_APP + 1;
static const UINT ID_SETTINGS = 1003, ID_QUIT = 1002;
static const UINT ID_HEADER = 1005;   // owner-drawn, disabled: the status readout
static const UINT ID_PROFILE_BASE = 1100;      // 1100..1131: one per profile menu item
static const UINT kMaxProfileMenuItems = 32;

static UINT DiagDoneMsg() { static UINT m = RegisterWindowMessageW(L"Wind.DiagnosticsExportDone.v1"); return m; }
static std::mutex  g_diagMx;
static std::wstring g_diagZip;
static bool g_diagOk = false, g_diagReady = false, g_diagRunning = false;

// Menu thread (2026-08-28, third design - the history matters here):
//   v1  SetTimer(8ms) kept the tick alive through TrackPopupMenu's modal loop, but SetTimer's
//       real floor is ~15.6ms, so the whole magnifier dropped to ~64Hz while any menu was open
//       (field-reported frame loss, and the runaway zoom-after-release in that degraded regime).
//   v2  a pump thread PostMessage'd WM_TIMER at 143Hz - and froze the menu solid, because POSTED
//       messages outrank HARDWARE input in the queue: the modal loop always found a posted tick
//       first and never dequeued the mouse.
//   v3  (this) the menu runs on ITS OWN THREAD with its own zero-size popup window. The main
//       thread never blocks, so the tick keeps its normal full-rate pacing with no keep-alive
//       trick at all, and the menu's input is handled by its own loop - both sides at full speed.
// One menu at a time (g_menuOpen); the weld is suspended while it is open (main.cpp reads
// MenuOpen()) so the pointer belongs to the user while they aim at menu items.
static void SwitchToProfile(const std::wstring& ini, const std::wstring& nameW);
static volatile LONG g_menuOpen = 0;
struct MenuCtx { HWND mainHwnd; POINT pt; };

// Owner-draw state for ONE menu open. All of it lives on the menu thread: items, fonts,
// palette, metrics are created per open and destroyed with the host window, and the status they
// render is read cross-thread from the tick loop's relaxed-atomic snapshot (tray_status.h) plus
// the frame-pacing ring (tick_stats.h) - both designed for exactly this reader.
struct MenuDrawState {
    std::vector<TrayDraw::Item> items;
    TrayDraw::Palette pal;
    TrayDraw::Metrics mt;
    TrayDraw::Fonts   fonts;
    // Live header (the menu would otherwise be a still - field-rejected): MenuThread sets `menu`
    // and a 100ms timer on the host; WM_TIMER finds the system's menu window once (class #32768
    // hosting `menu`) and invalidates the header item's rect, so WM_DRAWITEM re-reads the status
    // snapshot, the fps figure and the pacing ring while the menu is open.
    HMENU menu = nullptr;
    HWND  menuWnd = nullptr;
};

static void MeasureItem(const MenuDrawState& st, MEASUREITEMSTRUCT* mis) {
    if (!mis || mis->CtlType != ODT_MENU) return;
    const auto* it = reinterpret_cast<const TrayDraw::Item*>(mis->itemData);
    mis->itemWidth  = (UINT)st.mt.menuWidth();
    mis->itemHeight = (UINT)(!it ? st.mt.rowHeight()
                             : it->kind == TrayDraw::Kind::Header ? st.mt.headerH()
                             : it->kind == TrayDraw::Kind::Sep    ? st.mt.sepH()
                                                                  : st.mt.rowHeight());
}

static void DrawHeaderBody(const MenuDrawState& st, HDC dc, const RECT& r) {
    const auto& pal = st.pal;
    const int pad = st.mt.padX();
    const TrayStatus ts = ReadTrayStatus();
    wchar_t zoom[16];
    const bool zoomed = FormatZoom(ts.level, zoom, 16);

    HGDIOBJ of = SelectObject(dc, st.fonts.big);
    SetTextColor(dc, zoomed ? pal.text : pal.faint);
    RECT zr{ r.left + pad, r.top + st.mt.scale(12), r.right - pad, r.top + st.mt.scale(46) };
    DrawTextW(dc, zoom, -1, &zr, DT_LEFT | DT_TOP | DT_SINGLELINE);
    SIZE zs{}; GetTextExtentPoint32W(dc, zoom, lstrlenW(zoom), &zs);
    if (zoomed) {
        SelectObject(dc, st.fonts.body);
        SetTextColor(dc, pal.dim);
        RECT xr{ zr.left + zs.cx + st.mt.scale(3), r.top + st.mt.scale(28),
                 r.right - pad, r.top + st.mt.scale(48) };
        DrawTextW(dc, L"x", -1, &xr, DT_LEFT | DT_TOP | DT_SINGLELINE);
    }

    float buf[TickStats::kCap];
    const int n = Ticks().snapshot(buf, TickStats::kCap);
    if (n >= 8) {
        const double fps = FpsFromMs(MedianMs(buf, n));
        wchar_t f[32]; wsprintfW(f, L"%d fps", (int)(fps + 0.5));
        SelectObject(dc, st.fonts.small_);
        SetTextColor(dc, pal.faint);
        RECT kr{ r.left, r.top + st.mt.scale(13), r.right - pad, r.top + st.mt.scale(25) };
        DrawTextW(dc, L"COMPOSITION", -1, &kr, DT_RIGHT | DT_TOP | DT_SINGLELINE);
        SelectObject(dc, st.fonts.body);
        SetTextColor(dc, LateCount(buf, n) > 0 ? pal.warn : pal.ok);
        RECT vr{ r.left, r.top + st.mt.scale(26), r.right - pad, r.top + st.mt.scale(44) };
        DrawTextW(dc, f, -1, &vr, DT_RIGHT | DT_TOP | DT_SINGLELINE);
    }

    const wchar_t* eng = EngineLabel(ts.engine);
    SelectObject(dc, st.fonts.pill);
    SIZE ps{}; GetTextExtentPoint32W(dc, eng, lstrlenW(eng), &ps);
    RECT pill{ r.left + pad, r.top + st.mt.scale(50),
               r.left + pad + ps.cx + st.mt.scale(12), r.top + st.mt.scale(68) };
    TrayDraw::FillRoundRectC(dc, pill, pal.dark ? RGB(0x2e,0x2e,0x4a) : RGB(0xe6,0xe6,0xfa),
                             st.mt.scale(4));
    SetTextColor(dc, pal.accent);
    DrawTextW(dc, eng, -1, &pill, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, st.fonts.body);
    SetTextColor(dc, pal.dim);
    RECT sr{ pill.right + st.mt.scale(8), pill.top, r.right - pad, pill.bottom };
    DrawTextW(dc, zoomed ? (ts.panning ? L"active" : L"holding")
                         : L"Hold your zoom button", -1, &sr,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    RECT sp{ r.left + pad, r.top + st.mt.scale(74), r.right - pad, r.top + st.mt.scale(98) };
    TrayDraw::DrawSparkline(dc, sp, pal, st.mt);
    SelectObject(dc, of);
}

static void DrawItem(const MenuDrawState& st, DRAWITEMSTRUCT* dis) {
    if (!dis || dis->CtlType != ODT_MENU) return;
    const auto* it = reinterpret_cast<const TrayDraw::Item*>(dis->itemData);
    if (!it) return;
    HDC dc = dis->hDC;
    RECT r = dis->rcItem;
    const auto& pal = st.pal;
    const int pad = st.mt.padX();

    if (it->kind == TrayDraw::Kind::Header) {
        // Double-buffered: the live timer repaints the header ~10x a second, and menu windows
        // are not double-buffered - drawing text straight to the screen at that cadence shimmers.
        const int w = r.right - r.left, h = r.bottom - r.top;
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = mem ? CreateCompatibleBitmap(dc, w, h) : nullptr;
        if (mem && bmp) {
            HGDIOBJ ob = SelectObject(mem, bmp);
            SetWindowOrgEx(mem, r.left, r.top, nullptr);   // header code keeps its item coords
            SetBkMode(mem, TRANSPARENT);
            TrayDraw::FillRectC(mem, r, pal.bg);
            DrawHeaderBody(st, mem, r);
            BitBlt(dc, r.left, r.top, w, h, mem, r.left, r.top, SRCCOPY);
            SelectObject(mem, ob);
        } else {
            TrayDraw::FillRectC(dc, r, pal.bg);
            SetBkMode(dc, TRANSPARENT);
            DrawHeaderBody(st, dc, r);
        }
        if (bmp) DeleteObject(bmp);
        if (mem) DeleteDC(mem);
        return;
    }

    TrayDraw::FillRectC(dc, r, pal.bg);
    SetBkMode(dc, TRANSPARENT);

    if (it->kind == TrayDraw::Kind::Sep) {
        RECT ln{ r.left + pad, (r.top + r.bottom) / 2, r.right - pad, (r.top + r.bottom) / 2 + 1 };
        TrayDraw::FillRectC(dc, ln, pal.sep);
        return;
    }

    const bool sel = (dis->itemState & ODS_SELECTED) != 0;
    if (sel) {
        // Windows 11 selection language: rounded fill, short accent bar at the leading edge.
        RECT hr{ r.left + st.mt.scale(4), r.top + st.mt.scale(2),
                 r.right - st.mt.scale(4), r.bottom - st.mt.scale(2) };
        TrayDraw::FillRoundRectC(dc, hr, pal.hover, st.mt.scale(4));
        RECT bar{ hr.left, hr.top + st.mt.scale(6),
                  hr.left + st.mt.scale(3), hr.bottom - st.mt.scale(6) };
        TrayDraw::FillRoundRectC(dc, bar, pal.accent, st.mt.scale(1));
    }
    HGDIOBJ of = SelectObject(dc, st.fonts.body);
    SetTextColor(dc, pal.text);
    int textLeft = r.left + pad + st.mt.scale(10);
    if (it->kind == TrayDraw::Kind::Radio) {
        if (it->checked) {
            const int ccx = r.left + pad + st.mt.scale(5);
            const int ccy = (r.top + r.bottom) / 2;
            HBRUSH br = CreateSolidBrush(pal.accent);
            HGDIOBJ ob = SelectObject(dc, br);
            HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
            Ellipse(dc, ccx - st.mt.scale(3), ccy - st.mt.scale(3),
                        ccx + st.mt.scale(3), ccy + st.mt.scale(3));
            SelectObject(dc, op); SelectObject(dc, ob); DeleteObject(br);
        }
        textLeft = r.left + pad + st.mt.scale(16);
    }
    RECT tr{ textLeft, r.top, r.right - pad, r.bottom };
    DrawTextW(dc, it->label.c_str(), -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (!it->value.empty() || it->submenu) {
        SetTextColor(dc, pal.dim);
        RECT vr{ r.left, r.top, r.right - pad - (it->submenu ? st.mt.scale(16) : 0), r.bottom };
        if (!it->value.empty())
            DrawTextW(dc, it->value.c_str(), -1, &vr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        if (it->submenu) {
            SetTextColor(dc, pal.faint);
            RECT cr{ r.right - pad - st.mt.scale(10), r.top, r.right - pad, r.bottom };
            DrawTextW(dc, L"\u203A", -1, &cr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        }
    }
    SelectObject(dc, of);
}

// The host window's WndProc. The owner-drawn menu sends WM_MEASUREITEM / WM_DRAWITEM to the menu
// OWNER, which is this window on the MENU THREAD - the whole point of the port: the instrument
// drawing lives here and the main thread's tick never sees any of it.
static LRESULT CALLBACK MenuHostProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto* st = reinterpret_cast<MenuDrawState*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (st) {
        if (m == WM_MEASUREITEM) { MeasureItem(*st, reinterpret_cast<MEASUREITEMSTRUCT*>(l)); return TRUE; }
        if (m == WM_DRAWITEM)    { DrawItem(*st, reinterpret_cast<DRAWITEMSTRUCT*>(l));       return TRUE; }
        if (m == WM_TIMER && w == 1) {
            // The live tick. TrackPopupMenu's modal loop dispatches WM_TIMER here (unlike posted
            // messages, timers do not starve its input - the v1/v2 history above). Find the
            // system's menu window once, then invalidate just the header item so it redraws.
            if (!st->menuWnd && st->menu) {
                EnumThreadWindows(GetCurrentThreadId(), [](HWND wnd, LPARAM lp) -> BOOL {
                    auto* s2 = reinterpret_cast<MenuDrawState*>(lp);
                    wchar_t cls[16];
                    if (GetClassNameW(wnd, cls, 16) && lstrcmpW(cls, L"#32768") == 0 &&
                        reinterpret_cast<HMENU>(SendMessageW(wnd, MN_GETHMENU, 0, 0)) == s2->menu) {
                        s2->menuWnd = wnd;
                        // While we have the window: Windows 11 rounds its own menus, but an
                        // owner-drawn one shows square corners unless asked explicitly.
                        DWM_WINDOW_CORNER_PREFERENCE cp = DWMWCP_ROUND;
                        DwmSetWindowAttribute(wnd, DWMWA_WINDOW_CORNER_PREFERENCE, &cp, sizeof(cp));
                        return FALSE;
                    }
                    return TRUE;
                }, reinterpret_cast<LPARAM>(st));
            }
            if (st->menuWnd) {
                RECT hr{};
                if (GetMenuItemRect(nullptr, st->menu, 0, &hr)) {
                    MapWindowPoints(HWND_DESKTOP, st->menuWnd, reinterpret_cast<POINT*>(&hr), 2);
                    InvalidateRect(st->menuWnd, &hr, FALSE);
                } else {
                    InvalidateRect(st->menuWnd, nullptr, FALSE);
                }
            }
            return 0;
        }
    }
    return DefWindowProcW(h, m, w, l);
}

static DWORD WINAPI MenuThread(LPVOID param) {
    MenuCtx ctx = *static_cast<MenuCtx*>(param);
    delete static_cast<MenuCtx*>(param);

    // Registered class, not "STATIC": the owner-drawn header needs a WndProc that answers
    // WM_MEASUREITEM / WM_DRAWITEM, and a static control's default proc does not.
    static ATOM s_cls = 0;
    if (!s_cls) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = MenuHostProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"WindMenuHost";
        s_cls = RegisterClassW(&wc);
    }
    HWND host = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"WindMenuHost", L"",
                                WS_POPUP, ctx.pt.x, ctx.pt.y, 0, 0,
                                nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!host) { InterlockedExchange(&g_menuOpen, 0); return 0; }
    ShowWindow(host, SW_SHOWNOACTIVATE);

    // Per-open draw state: theme, DPI and fonts are not constants (system theme switch,
    // per-monitor DPI), so they are resolved fresh on every open.
    MenuDrawState st;
    st.pal = TrayDraw::MakePalette();
    st.mt.dpi = (int)GetDpiForWindow(host);
    if (st.mt.dpi <= 0) st.mt.dpi = 96;
    st.fonts.create(st.mt.dpi);
    SetWindowLongPtrW(host, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&st));

    const std::wstring ini = wind::ResolveIniPath();
    std::vector<std::wstring> profNames = wind::ListProfileFiles(wind::ProfilesDirFromIni(ini));
    if (profNames.size() > kMaxProfileMenuItems) profNames.resize(kMaxProfileMenuItems);
    const std::wstring active = wind::WidenUtf8(
        wind::ReadIniValues(wind::ReadTextFile(ini))["profile"]);

    // Item models BEFORE appending, never resized after: the menu holds raw pointers into this
    // vector for as long as it is open.
    st.items.reserve(profNames.size() + 6);
    st.items.push_back({ TrayDraw::Kind::Header, L"", L"", false, false });
    st.items.push_back({ TrayDraw::Kind::Action, L"Profile",
                         active.empty() ? std::wstring(L"Default") : active, false, true });
    st.items.push_back({ TrayDraw::Kind::Action, L"Settings", L"", false, false });
    st.items.push_back({ TrayDraw::Kind::Action, L"Quit",     L"", false, false });
    st.items.push_back({ TrayDraw::Kind::Sep, L"", L"", false, false });   // [4]
    st.items.push_back({ TrayDraw::Kind::Sep, L"", L"", false, false });   // [5]
    const size_t profFirst = st.items.size();
    for (const auto& nm : profNames)
        st.items.push_back({ TrayDraw::Kind::Radio, nm, L"",
                             _wcsicmp(nm.c_str(), active.c_str()) == 0, false });

    HMENU m = CreatePopupMenu();
    HMENU pm = CreatePopupMenu();
    for (UINT i = 0; i < (UINT)profNames.size(); ++i)
        AppendMenuW(pm, MF_OWNERDRAW, ID_PROFILE_BASE + i,
                    reinterpret_cast<LPCWSTR>(&st.items[profFirst + i]));

    AppendMenuW(m, MF_OWNERDRAW | MF_DISABLED | MF_GRAYED, ID_HEADER,
                reinterpret_cast<LPCWSTR>(&st.items[0]));
    AppendMenuW(m, MF_OWNERDRAW | MF_DISABLED, 0, reinterpret_cast<LPCWSTR>(&st.items[4]));
    if (!profNames.empty())
        AppendMenuW(m, MF_OWNERDRAW | MF_POPUP, reinterpret_cast<UINT_PTR>(pm),
                    reinterpret_cast<LPCWSTR>(&st.items[1]));
    else
        DestroyMenu(pm);
    AppendMenuW(m, MF_OWNERDRAW, ID_SETTINGS, reinterpret_cast<LPCWSTR>(&st.items[2]));
    AppendMenuW(m, MF_OWNERDRAW | MF_DISABLED, 0, reinterpret_cast<LPCWSTR>(&st.items[5]));
    AppendMenuW(m, MF_OWNERDRAW, ID_QUIT, reinterpret_cast<LPCWSTR>(&st.items[3]));

    // Our background too, or the system paints the gaps around the owner-drawn rows in the
    // default colour and the panel reads as two different widgets.
    MENUINFO mi{}; mi.cbSize = sizeof(mi);
    mi.fMask = MIM_BACKGROUND | MIM_APPLYTOSUBMENUS;
    mi.hbrBack = CreateSolidBrush(st.pal.bg);
    SetMenuInfo(m, &mi);

    st.menu = m;
    SetTimer(host, 1, 100, nullptr);   // the live-header tick; see MenuHostProc
    SetForegroundWindow(host);   // we own the last input (the tray click), so this is permitted
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, ctx.pt.x, ctx.pt.y, 0,
                             host, nullptr);
    KillTimer(host, 1);
    PostMessageW(host, WM_NULL, 0, 0);   // the documented dismiss fix
    SetWindowLongPtrW(host, GWLP_USERDATA, 0);
    DestroyMenu(m);
    if (mi.hbrBack) DeleteObject(mi.hbrBack);
    st.fonts.destroy();
    DestroyWindow(host);

    // Actions - all safe off the main thread: ShellExecute is thread-agnostic, Quit is a
    // PostMessage to the main window, and the profile switch is file I/O whose UI feedback
    // (Notify) is a process-global Shell_NotifyIcon call. Export diagnostics left the tray with
    // this design (it is a button in the Settings UI); its worker plumbing stays for the
    // Settings-origin path.
    if (cmd == ID_SETTINGS)
        ShellExecuteW(nullptr, L"open", L"WindConfig.exe", nullptr, nullptr, SW_SHOW);
    else if (cmd == ID_QUIT)
        PostMessageW(ctx.mainHwnd, WM_CLOSE, 0, 0);
    else if (cmd >= (int)ID_PROFILE_BASE && cmd < (int)(ID_PROFILE_BASE + profNames.size()))
        SwitchToProfile(ini, profNames[cmd - ID_PROFILE_BASE]);

    InterlockedExchange(&g_menuOpen, 0);
    return 0;
}

// Diagnostics-export completion signal. The worker thread does NOT smuggle a heap pointer through the
// window message (any local process could PostMessage a forged LPARAM -> controlled deref/free). Instead
// it parks the result in this mutex-guarded slot and posts a bare wake-up; the handler reads the slot
// under the lock and never dereferences the message params. The message id is registered (process-unique,
// >= 0xC000) so it isn't a guessable WM_APP+n, and the handler ignores any wake-up with no result ready.

void Add(HWND hwnd, HINSTANCE hInst) {
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY;
    // Our logo badge at the shell's small-icon size (picks the 16px frame from the multi-size .ico
    // for a crisp tray render). Fall back to the generic app icon if the resource can't be loaded.
    if (!hInst) hInst = GetModuleHandleW(nullptr);
    g_nid.hIcon = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_WIND), IMAGE_ICON,
                                    GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
                                    LR_DEFAULTCOLOR);
    if (!g_nid.hIcon) g_nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    lstrcpyW(g_nid.szTip, L"Wind magnifier");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}
void Remove() { Shell_NotifyIconW(NIM_DELETE, &g_nid); }
void Notify(const wchar_t* title, const wchar_t* text) {
    g_nid.uFlags = NIF_INFO;
    // Bounded copies: szInfoTitle is 64 wchars, szInfo 256; profile names travel through here,
    // so an unbounded lstrcpyW was a caller-controlled overflow of the fixed NOTIFYICONDATA.
    lstrcpynW(g_nid.szInfoTitle, title, ARRAYSIZE(g_nid.szInfoTitle));
    lstrcpynW(g_nid.szInfo, text, ARRAYSIZE(g_nid.szInfo));
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
}
// Switch the active profile from the tray: rewrite the live ini from the profile file (globals
// preserved); the core's dir-watch hot-reloads everything except `model`, which is read once at
// launch - a model change relaunches Wind.exe (the new instance evicts us via the single-instance
// handshake, same as the swap-model path in main.cpp).
static void SwitchToProfile(const std::wstring& ini, const std::wstring& nameW) {
    // Every step logs (issue #184: a field switch failed with no trace - the balloon is
    // transient, the log is not).
    wind::Log(wind::LogLevel::Info, "profile", "tray switch -> %ls", nameW.c_str());
    const std::wstring profPath = wind::ProfilesDirFromIni(ini) + L"\\" + nameW + L".ini";
    if (GetFileAttributesW(profPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        wind::Log(wind::LogLevel::Warn, "profile", "switch aborted: file missing");
        Notify(L"Wind", L"That profile's file is missing; settings unchanged.");
        return;
    }
    // A read failure must NOT masquerade as an empty profile: empty legitimately means factory
    // defaults, so silently treating a locked/corrupt file as empty would wipe the live settings.
    std::string profText;
    if (!wind::ReadTextFileOk(profPath, profText)) {
        wind::Log(wind::LogLevel::Warn, "profile", "switch aborted: read failed (err=%lu)", GetLastError());
        Notify(L"Wind", L"Could not read that profile's file; settings unchanged.");
        return;
    }
    { std::string terr = wind::ProfileTextError(profText);
      if (!terr.empty()) {
        wind::Log(wind::LogLevel::Warn, "profile", "switch aborted: %s", terr.c_str());
        Notify(L"Wind", L"That profile's file looks corrupt; settings unchanged.");
        return;
    } }
    const std::string oldLive = wind::ReadTextFile(ini);
    // Capture hand edits (openIni) into the outgoing profile before the live ini is replaced.
    wind::MirrorLiveToActiveProfile(ini, oldLive);
    const std::string newLive = wind::MakeLiveText(profText, oldLive, wind::NarrowUtf8(nameW));
    if (!wind::WriteTextFileAtomic(ini, newLive)) {
        wind::Log(wind::LogLevel::Warn, "profile", "switch aborted: live ini write failed (err=%lu)", GetLastError());
        Notify(L"Wind", L"Could not switch profile (config file is locked).");
        return;
    }
    // Model is not hot-swappable: relaunch so the new instance boots on the profile's model.
    const std::string oldModel = wind::ParseConfig(oldLive).model;
    const std::string newModel = wind::ParseConfig(newLive).model;
    wind::Log(wind::LogLevel::Info, "profile", "switch applied: model %s -> %s%s",
              oldModel.c_str(), newModel.c_str(),
              oldModel != newModel ? " (relaunching)" : " (hot)");
    if (oldModel != newModel) {
        wchar_t exe[MAX_PATH];
        const bool haveExe = GetModuleFileNameW(nullptr, exe, MAX_PATH) != 0;
        INT_PTR rc = haveExe
            ? reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", exe, nullptr, nullptr,
                                                      SW_SHOWNORMAL))
            : 0;
        if (rc > 32) {
            Notify(L"Wind", (L"Switched to \"" + nameW + L"\" (restarting for its model).").c_str());
        } else {
            // Keep the "ini model == running model" invariant (same rule as the Settings
            // restartFailed path): the switch stays, only the model is kept.
            wind::Log(wind::LogLevel::Warn, "profile",
                      "relaunch FAILED (rc=%lld haveExe=%d); reverting model to %s",
                      static_cast<long long>(rc), (int)haveExe, oldModel.c_str());
            wind::WriteTextFileAtomic(ini, wind::UpdateIniText(newLive, "model", oldModel));
            Notify(L"Wind", L"Profile switched; kept the current model (restart failed).");
        }
    }
}

bool MenuOpen() { return InterlockedCompareExchange(&g_menuOpen, 0, 0) != 0; }

bool HandleMessage(HWND hwnd, UINT msg, WPARAM /*wp*/, LPARAM lp) {
    if (msg == DiagDoneMsg()) {
        // Export worker finished (off-thread). Read the result from the guarded slot - the message params
        // are NOT trusted/dereferenced, so a forged wake-up from another process can't deref a pointer.
        std::wstring zip; bool ok = false, ready = false;
        { std::lock_guard<std::mutex> lk(g_diagMx);
          if (g_diagReady) { zip = std::move(g_diagZip); ok = g_diagOk; g_diagReady = false; g_diagZip.clear(); ready = true; } }
        if (!ready) return true;   // no export result pending (spurious/foreign wake-up): ignore
        if (ok && !zip.empty()) {  // reveal + notify here, on the message thread (tray state stays single-threaded)
            std::wstring args = L"/select,\"" + zip + L"\"";
            ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
            Notify(L"Wind", L"Diagnostics exported to your Desktop.");
        } else {
            Notify(L"Wind", L"Could not export diagnostics.");
        }
        return true;
    }
    if (msg == WM_TRAY && (lp == WM_RBUTTONUP || lp == WM_LBUTTONUP)) {
        // One menu at a time; a second click while it is open is just a re-click, ignored.
        if (InterlockedCompareExchange(&g_menuOpen, 1, 0) != 0) return true;
        auto* ctx = new MenuCtx{};
        ctx->mainHwnd = hwnd;
        GetCursorPos(&ctx->pt);
        HANDLE t = CreateThread(nullptr, 0, MenuThread, ctx, 0, nullptr);
        if (t) CloseHandle(t);
        else { delete ctx; InterlockedExchange(&g_menuOpen, 0); }
        return true;
    }
    if (msg == WM_CLOSE)  { DestroyWindow(hwnd); return true; }
    if (msg == WM_DESTROY) { PostQuitMessage(0); return true; }
    return false;
}
}}
