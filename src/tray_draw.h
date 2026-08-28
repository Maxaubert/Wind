#pragma once
// Owner-drawn tray menu: the drawing half, kept out of tray.cpp so the menu logic there stays
// readable. See docs/superpowers/specs/2026-08-28-tray-menu-design.md for the design and for why
// this is an owner-drawn HMENU rather than a custom popup window.
#include <windows.h>
#include <string>
#include <vector>
#include "tick_stats.h"
#include "tray_status.h"

namespace wind { namespace TrayDraw {

enum class Kind { Header, Action, Radio };

struct Item {
    Kind         kind = Kind::Action;
    std::wstring label;      // "Settings"
    std::wstring value;      // right-aligned secondary text, e.g. the active profile name
    bool         checked = false;   // Radio only
    bool         submenu = false;   // draws the chevron
};

// Palette resolved once per menu open. The system theme is the authority; the accent is ours.
struct Palette {
    bool     dark = true;
    COLORREF bg, text, dim, faint, sep, hover, accent, ok, warn;
};

inline bool SystemUsesLightTheme() {
    DWORD v = 0, cb = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER,
                     L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &cb) == ERROR_SUCCESS)
        return v != 0;
    return false;   // no key: assume dark, which is what Wind's own UI defaults to
}

inline Palette MakePalette() {
    Palette p;
    p.dark   = !SystemUsesLightTheme();
    p.accent = RGB(0x5b, 0x5b, 0xd6);          // Wind accent, both themes
    p.ok     = RGB(0x4a, 0xde, 0x80);
    p.warn   = RGB(0xfb, 0xbf, 0x24);
    if (p.dark) {
        p.bg = RGB(0x20,0x20,0x20); p.text = RGB(0xf2,0xf2,0xf2); p.dim = RGB(0x9a,0x9a,0x9a);
        p.faint = RGB(0x6e,0x6e,0x6e); p.sep = RGB(0x2e,0x2e,0x2e); p.hover = RGB(0x2b,0x2b,0x2b);
    } else {
        p.bg = RGB(0xf9,0xf9,0xf9); p.text = RGB(0x1a,0x1a,0x1a); p.dim = RGB(0x5f,0x5f,0x5f);
        p.faint = RGB(0x8a,0x8a,0x8a); p.sep = RGB(0xe5,0xe5,0xe5); p.hover = RGB(0xec,0xec,0xec);
    }
    return p;
}

// Every metric is DPI-scaled; nothing here is a raw pixel count at the call site.
struct Metrics {
    int dpi = 96;
    int scale(int v) const { return MulDiv(v, dpi, 96); }
    int menuWidth()  const { return scale(300); }
    int rowHeight()  const { return scale(34); }
    int headerH()    const { return scale(104); }
    int padX()       const { return scale(12); }
};

// Fonts for one menu open. The body font is the shell's own menu font so the items match every
// other menu on the system; only the headline figure departs from it, and deliberately.
struct Fonts {
    HFONT body = nullptr, big = nullptr, small_ = nullptr, pill = nullptr;
    void create(int dpi) {
        NONCLIENTMETRICSW ncm{}; ncm.cbSize = sizeof(ncm);
        LOGFONTW lf{};
        if (SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, (UINT)dpi))
            lf = ncm.lfMenuFont;
        else { lstrcpynW(lf.lfFaceName, L"Segoe UI", 32); lf.lfHeight = -MulDiv(9, dpi, 72); }
        body = CreateFontIndirectW(&lf);
        LOGFONTW b = lf; b.lfHeight = -MulDiv(21, dpi, 72); b.lfWeight = FW_SEMIBOLD;
        big = CreateFontIndirectW(&b);
        LOGFONTW s = lf; s.lfHeight = -MulDiv(7, dpi, 72);
        small_ = CreateFontIndirectW(&s);
        LOGFONTW q = lf; q.lfHeight = -MulDiv(7, dpi, 72); q.lfWeight = FW_BOLD;
        pill = CreateFontIndirectW(&q);
    }
    void destroy() {
        if (body) DeleteObject(body); if (big) DeleteObject(big);
        if (small_) DeleteObject(small_); if (pill) DeleteObject(pill);
        body = big = small_ = pill = nullptr;
    }
};

inline void FillRectC(HDC dc, const RECT& r, COLORREF c) {
    HBRUSH b = CreateSolidBrush(c); FillRect(dc, &r, b); DeleteObject(b);
}

// The frame-pacing sparkline. Drawn from a snapshot of the tick ring, so it is a still: the values
// are current at the moment the menu opened, which is all a menu needs to be truthful.
inline void DrawSparkline(HDC dc, const RECT& box, const Palette& pal, const Metrics& mt) {
    FillRectC(dc, box, pal.dark ? RGB(0x19,0x19,0x19) : RGB(0xf0,0xf0,0xf0));
    HPEN edge = CreatePen(PS_SOLID, 1, pal.sep);
    HGDIOBJ oldp = SelectObject(dc, edge);
    HGDIOBJ oldb = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, box.left, box.top, box.right, box.bottom);
    SelectObject(dc, oldb); SelectObject(dc, oldp); DeleteObject(edge);

    float buf[TickStats::kCap];
    const int n = Ticks().snapshot(buf, TickStats::kCap);
    const int w = box.right - box.left, h = box.bottom - box.top;
    if (n < 8 || w <= 4 || h <= 4) {
        // Idle: a flat dashed rule rather than an empty box, so the menu keeps its height and does
        // not appear broken when there is simply nothing to plot.
        HPEN dash = CreatePen(PS_DOT, 1, pal.faint);
        HGDIOBJ o = SelectObject(dc, dash);
        MoveToEx(dc, box.left + 2, (box.top + box.bottom) / 2, nullptr);
        LineTo(dc, box.right - 2, (box.top + box.bottom) / 2);
        SelectObject(dc, o); DeleteObject(dash);
        return;
    }
    // Scale to 2x the median so a normal frame sits mid-box and a stall is unmistakable, rather
    // than autoscaling to the max (which makes every graph look equally bad).
    const double med = MedianMs(buf, n);
    const double top = med > 0.01 ? med * 2.0 : 20.0;
    HPEN line = CreatePen(PS_SOLID, mt.scale(1), pal.accent);
    HGDIOBJ o = SelectObject(dc, line);
    for (int i = 0; i < n; ++i) {
        const int x = box.left + 2 + MulDiv(i, w - 4, n - 1);
        double v = buf[i] / top; if (v > 1.0) v = 1.0;
        const int y = box.bottom - 2 - (int)(v * (h - 4));
        if (i == 0) MoveToEx(dc, x, y, nullptr); else LineTo(dc, x, y);
    }
    SelectObject(dc, o); DeleteObject(line);
}

}}  // namespace wind::TrayDraw
