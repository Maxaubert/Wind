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

enum class Kind { Header, Action, Radio, Sep };

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
        p.faint = RGB(0x6e,0x6e,0x6e); p.sep = RGB(0x33,0x33,0x33); p.hover = RGB(0x2d,0x2d,0x2d);
    } else {
        p.bg = RGB(0xf9,0xf9,0xf9); p.text = RGB(0x1a,0x1a,0x1a); p.dim = RGB(0x5f,0x5f,0x5f);
        p.faint = RGB(0x8a,0x8a,0x8a); p.sep = RGB(0xe5,0xe5,0xe5); p.hover = RGB(0xec,0xec,0xec);
    }
    return p;
}

// GDI has no alpha; a fixed blend toward another colour is how the panel gets its tints.
inline COLORREF Blend(COLORREF a, COLORREF b, int pctA) {
    const int q = 100 - pctA;
    return RGB((GetRValue(a) * pctA + GetRValue(b) * q) / 100,
               (GetGValue(a) * pctA + GetGValue(b) * q) / 100,
               (GetBValue(a) * pctA + GetBValue(b) * q) / 100);
}

// Every metric is DPI-scaled; nothing here is a raw pixel count at the call site.
struct Metrics {
    int dpi = 96;
    int scale(int v) const { return MulDiv(v, dpi, 96); }
    int menuWidth()  const { return scale(300); }
    int rowHeight()  const { return scale(34); }
    int headerH()    const { return scale(104); }
    int sepH()       const { return scale(9); }
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

inline void FillRoundRectC(HDC dc, const RECT& r, COLORREF c, int rad) {
    HBRUSH b = CreateSolidBrush(c); HPEN p = CreatePen(PS_SOLID, 1, c);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, rad * 2, rad * 2);
    SelectObject(dc, op); SelectObject(dc, ob); DeleteObject(p); DeleteObject(b);
}

// The frame-pacing sparkline. LIVE while the menu is open (the host window's timer invalidates
// the header, so this redraws from the current ring ~10x a second). Drawn as a filled area with
// a bright top line - the original 1px accent hairline on the dark well was field-rejected as
// invisible. The dotted midline IS the median (the scale is 2x median), so a healthy trace hugs
// it and a stall spikes visibly above it.
inline void DrawSparkline(HDC dc, const RECT& box, const Palette& pal, const Metrics& mt) {
    const COLORREF well = pal.dark ? RGB(0x19,0x19,0x1c) : RGB(0xef,0xef,0xf3);
    FillRoundRectC(dc, box, well, mt.scale(4));

    float buf[TickStats::kCap];
    const int n = Ticks().snapshot(buf, TickStats::kCap);
    const int w = box.right - box.left, h = box.bottom - box.top;
    const int midY = (box.top + box.bottom) / 2;
    if (n < 8 || w <= 4 || h <= 4) {
        // Idle: a flat dashed rule rather than an empty box, so the menu keeps its height and does
        // not appear broken when there is simply nothing to plot.
        HPEN dash = CreatePen(PS_DOT, 1, pal.faint);
        HGDIOBJ o = SelectObject(dc, dash);
        MoveToEx(dc, box.left + 2, midY, nullptr);
        LineTo(dc, box.right - 2, midY);
        SelectObject(dc, o); DeleteObject(dash);
        return;
    }
    // Scale to 2x the median so a normal frame sits mid-box and a stall is unmistakable, rather
    // than autoscaling to the max (which makes every graph look equally bad).
    const double med = MedianMs(buf, n);
    const double top = med > 0.01 ? med * 2.0 : 20.0;

    HPEN grid = CreatePen(PS_DOT, 1, Blend(pal.faint, well, 55));
    HGDIOBJ og = SelectObject(dc, grid);
    MoveToEx(dc, box.left + 2, midY, nullptr);
    LineTo(dc, box.right - 2, midY);
    SelectObject(dc, og); DeleteObject(grid);

    // This is a STALL METER, not an oscilloscope. The ring records when the LOOP WOKE, so it
    // carries the scheduler's wake jitter: a wake 1-2ms late plus its short catch-up tick, in
    // up/down pairs (a 12% deadband still let those poke through - field 2026-08-28; RTSS reads
    // flat because it plots the vsync-locked present intervals). None of that can miss a
    // composite. The honest cut is the tick that CAN: anything under 1.5x the median collapses
    // to the median line, so the trace is flat while healthy and spikes only for a real stall.
    POINT pts[TickStats::kCap + 2];
    for (int i = 0; i < n; ++i) {
        pts[i].x = box.left + 2 + MulDiv(i, w - 4, n - 1);
        const double raw = (buf[i] < med * 1.5) ? med : buf[i];
        double v = raw / top; if (v > 1.0) v = 1.0;
        pts[i].y = box.bottom - 2 - (LONG)(v * (h - 4));
    }
    pts[n]     = { box.right - 2, box.bottom - 2 };
    pts[n + 1] = { box.left + 2,  box.bottom - 2 };
    HBRUSH fill = CreateSolidBrush(Blend(pal.accent, well, 30));
    HGDIOBJ ofb = SelectObject(dc, fill), ofp = SelectObject(dc, GetStockObject(NULL_PEN));
    Polygon(dc, pts, n + 2);
    SelectObject(dc, ofp); SelectObject(dc, ofb); DeleteObject(fill);

    HPEN line = CreatePen(PS_SOLID, mt.scale(1), Blend(pal.accent, RGB(0xff,0xff,0xff), 65));
    HGDIOBJ o = SelectObject(dc, line);
    Polyline(dc, pts, n);
    SelectObject(dc, o); DeleteObject(line);
}

}}  // namespace wind::TrayDraw
