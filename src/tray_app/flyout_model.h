#pragma once
// The tray flyout's PURE model (no <windows.h>, so it is unit-tested): where the window opens
// (taskbar on any edge, any monitor, any DPI), the layout of its rows in DIPs, hit-testing, the
// slider specs and value text, and the view model built from the ini, the tray layout and Wind's
// shared status. flyout_draw.cpp paints a View; flyout_window.cpp owns the window.
// Spec: docs/superpowers/specs/2026-10-01-tray-flyout-design.md (issue #313).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include "../tick_stats.h"
#include "../tray_items.h"
#include "../tray_status.h"
#include "flyout_tools.h"

namespace wind { namespace Flyout {

// ---------------------------------------------------------------- geometry primitives

struct IRect {
    int l = 0, t = 0, r = 0, b = 0;
    int w() const { return r - l; }
    int h() const { return b - t; }
    bool contains(int x, int y) const { return x >= l && x < r && y >= t && y < b; }
};

inline int ScalePx(int dip, int dpi) { return (dip * dpi + 48) / 96; }    // 96 dpi = 1:1, rounded
inline int ToDip(int px, int dpi) { return dpi > 0 ? (px * 96 + dpi / 2) / dpi : px; }

// ---------------------------------------------------------------- placement

enum class Edge { Bottom, Top, Left, Right };
struct Placement { int x = 0, y = 0; Edge edge = Edge::Bottom; };

inline int ClampInt(int v, int lo, int hi) {
    if (hi < lo) return lo;           // not enough room: keep the top/left edge on screen
    return v < lo ? lo : (v > hi ? hi : v);
}

// Which edge the taskbar is on, from the part of the monitor the work area does not cover. With no
// visible taskbar (auto-hide, or an icon on another monitor) it falls back to the edge nearest the
// tray icon, which is where the shell will slide the bar in.
inline Edge DetectEdge(const IRect& monitor, const IRect& work, const IRect& icon) {
    const int dl = work.l - monitor.l, dt = work.t - monitor.t;
    const int dr = monitor.r - work.r, db = monitor.b - work.b;
    const int m = (std::max)((std::max)(dl, dr), (std::max)(dt, db));
    if (m > 0) {
        if (db == m) return Edge::Bottom;
        if (dt == m) return Edge::Top;
        if (dr == m) return Edge::Right;
        return Edge::Left;
    }
    const int cx = (icon.l + icon.r) / 2, cy = (icon.t + icon.b) / 2;
    const int toL = cx - monitor.l, toR = monitor.r - cx, toT = cy - monitor.t, toB = monitor.b - cy;
    const int n = (std::min)((std::min)(toL, toR), (std::min)(toT, toB));
    if (n == toB) return Edge::Bottom;
    if (n == toT) return Edge::Top;
    if (n == toR) return Edge::Right;
    return Edge::Left;
}

// Top-left of a w x h window (pixels) next to the tray icon: just inside the work area on the
// taskbar's side, centred on the icon along the taskbar, clamped to the work area, `gap` px from
// every edge. All arguments are physical pixels of the icon's monitor.
inline Placement PlaceFlyout(const IRect& icon, const IRect& monitor, const IRect& work,
                             int w, int h, int gap) {
    Placement p;
    p.edge = DetectEdge(monitor, work, icon);
    const int cx = (icon.l + icon.r) / 2, cy = (icon.t + icon.b) / 2;
    switch (p.edge) {
        case Edge::Bottom: p.x = cx - w / 2; p.y = work.b - h - gap; break;
        case Edge::Top:    p.x = cx - w / 2; p.y = work.t + gap; break;
        case Edge::Left:   p.x = work.l + gap; p.y = cy - h / 2; break;
        case Edge::Right:  p.x = work.r - w - gap; p.y = cy - h / 2; break;
    }
    p.x = ClampInt(p.x, work.l + gap, work.r - w - gap);
    p.y = ClampInt(p.y, work.t + gap, work.b - h - gap);
    return p;
}

// Top-left of a w x h window opened at the click point, the way the old TrackPopupMenu tray menu
// was (Max, 2026-10-02: "same position as the old tray menu", including when the icon lives in the
// overflow flyout). Max's choice (2026-10-02): the window opens UP AND TO THE LEFT of the pointer,
// its bottom-right corner on the point. It flips right only when there is no room on the left, and
// down only when there is no room above, then is clamped to the monitor. Physical pixels.
inline Placement PlaceAtPoint(int px, int py, const IRect& monitor, int w, int h) {
    Placement p;
    p.x = px - w; p.y = py - h;
    if (p.x < monitor.l) p.x = px;
    if (p.y < monitor.t) p.y = py;
    p.x = ClampInt(p.x, monitor.l, monitor.r - w);
    p.y = ClampInt(p.y, monitor.t, monitor.b - h);
    return p;
}

// Dismissal: a click on the tray icon while the flyout is open first DEACTIVATES it (the flyout
// closes, on button DOWN), then the icon's own click arrives (WM_TRAY on button UP, legacy
// callback) and would reopen it. Two cases make that up-click the same click:
//  - the button was still held, over the icon, at deactivation (pressHeldOnIcon): the up-click ends
//    that press however long it was held, bounded by kHeldClickMaxMs so a stale stamp never
//    swallows a later genuine click;
//  - a quick click whose up arrives within kReopenGuardMs of the deactivation.
inline constexpr unsigned long long kReopenGuardMs = 300;
inline constexpr unsigned long long kHeldClickMaxMs = 10000;
inline bool IgnoreIconClick(unsigned long long nowMs, unsigned long long deactivatedMs, bool pressHeldOnIcon) {
    if (deactivatedMs == 0 || nowMs < deactivatedMs) return false;
    const unsigned long long limit = pressHeldOnIcon ? kHeldClickMaxMs : kReopenGuardMs;
    return nowMs - deactivatedMs < limit;
}

// ---------------------------------------------------------------- layout (DIPs, window origin)

inline constexpr int kWidth = 300, kBorder = 1, kRadius = 10;
inline constexpr int kHeadH = 96;          // 18 pad + 28 big + 14 gap + 14 frame row + 22 pad
inline constexpr int kRowH = 40, kQsPadY = 8, kPadX = 20, kIcon = 16, kIconGap = 12, kValueW = 48;
// The control area (mockup v02, Max 2026-10-02): ONE segmented toggle group, a full-width 32 px bar,
// then the engine dropdown, a full-width 32 px field, 8 px below it. 6 px above the group, 14 px
// of padding under the last control.
inline constexpr int kSegH = 32, kSegLine = 1, kSegRadius = 8, kCtlTop = 6, kCtlGap = 8, kQsPadBottom = 14;
inline constexpr int kBarH = 40, kBtn = 32, kBottomPad = 8;

struct Geometry {
    int width = kWidth, height = 0;
    bool hasHead = false, hasQs = false;
    IRect head, qs, bar;
    std::vector<int> hair;                       // y of each 1 px rule
    std::vector<IRect> sliderRow, sliderIcon, sliderTrack, sliderValue;
    IRect segBar;                                // the whole toggle group (empty when no toggle is on)
    std::vector<IRect> seg;                      // one stretched segment per toggle
    IRect engine;                                // the engine dropdown trigger (empty when it is off)
    IRect profileBtn, settingsBtn, quitBtn;
};

// `profileTextW` is the measured width of the profile name in DIPs (the renderer measures it).
// `nToggles` segments fill the group; `hasEngine` adds the dropdown under it.
inline Geometry ComputeGeometry(bool perf, int nSliders, int nToggles, bool hasEngine, int profileTextW) {
    if (nToggles < 0) nToggles = 0;
    Geometry g;
    const int x0 = kBorder, x1 = kWidth - kBorder;
    int y = kBorder;
    bool first = true;
    auto rule = [&]() {
        if (!first) { g.hair.push_back(y); y += 1; }
        first = false;
    };
    if (perf) {
        rule();
        g.hasHead = true;
        g.head = { x0, y, x1, y + kHeadH };
        y += kHeadH;
    }
    const bool controls = nToggles > 0 || hasEngine;
    if (nSliders > 0 || controls) {
        rule();
        g.hasQs = true;
        const int top = y;
        int ry = y + kQsPadY;
        for (int i = 0; i < nSliders; ++i) {
            const int mid = ry + kRowH / 2;
            g.sliderRow.push_back({ x0, ry, x1, ry + kRowH });
            g.sliderIcon.push_back({ x0 + kPadX, mid - kIcon / 2, x0 + kPadX + kIcon, mid + kIcon / 2 });
            g.sliderValue.push_back({ x1 - kPadX - kValueW, ry, x1 - kPadX, ry + kRowH });
            g.sliderTrack.push_back({ x0 + kPadX + kIcon + kIconGap, mid - 3,
                                      x1 - kPadX - kValueW - kIconGap, mid + 3 });
            ry += kRowH;
        }
        if (controls) {
            const int cl = x0 + kPadX, cr = x1 - kPadX;       // the content width
            ry += kCtlTop;
            if (nToggles > 0) {
                for (const SegRect& r : LayoutSegments(nToggles, cl, cr, ry, kSegH, kSegLine))
                    g.seg.push_back({ r.l, r.t, r.r, r.b });
                g.segBar = { cl, ry, cr, ry + kSegH };
                ry += kSegH;
                if (hasEngine) ry += kCtlGap;
            }
            if (hasEngine) {
                g.engine = { cl, ry, cr, ry + kSegH };
                ry += kSegH;
            }
            ry += kQsPadBottom;
        } else {
            ry += kQsPadY;
        }
        g.qs = { x0, top, x1, ry };
        y = ry;
    }
    rule();
    g.bar = { x0, y, x1, y + kBarH };
    const int mid = y + kBarH / 2;
    g.quitBtn = { x1 - 12 - kBtn, mid - kBtn / 2, x1 - 12, mid + kBtn / 2 };
    g.settingsBtn = { g.quitBtn.l - 4 - kBtn, mid - kBtn / 2, g.quitBtn.l - 4, mid + kBtn / 2 };
    const int pl = x0 + 12;
    int pr = pl + 8 + kIcon + kIconGap + (std::max)(0, profileTextW) + 8;
    pr = (std::min)(pr, g.settingsBtn.l - 4);
    g.profileBtn = { pl, mid - kBtn / 2, pr, mid + kBtn / 2 };
    y += kBarH;
    g.height = y + kBottomPad + kBorder;
    return g;
}

// No engine dropdown (sliders-only cases and the unit tests).
inline Geometry ComputeGeometry(bool perf, int nSliders, int nToggles, int profileTextW) {
    return ComputeGeometry(perf, nSliders, nToggles, false, profileTextW);
}

enum class HitKind { None, Slider, Toggle, Engine, Profile, Settings, Quit };
struct Hit {
    HitKind kind = HitKind::None;
    int index = -1;
    bool operator==(const Hit& o) const { return kind == o.kind && index == o.index; }
    bool operator!=(const Hit& o) const { return !(*this == o); }
};

// x, y in DIPs from the window's top-left.
inline Hit HitTest(const Geometry& g, int x, int y) {
    for (size_t i = 0; i < g.seg.size(); ++i)
        if (g.seg[i].contains(x, y)) return { HitKind::Toggle, (int)i };
    if (g.engine.w() > 0 && g.engine.contains(x, y)) return { HitKind::Engine, 0 };
    for (size_t i = 0; i < g.sliderRow.size(); ++i)
        if (g.sliderRow[i].contains(x, y)) return { HitKind::Slider, (int)i };
    if (g.quitBtn.contains(x, y)) return { HitKind::Quit, 0 };
    if (g.settingsBtn.contains(x, y)) return { HitKind::Settings, 0 };
    if (g.profileBtn.contains(x, y)) return { HitKind::Profile, 0 };
    return {};
}

// ---------------------------------------------------------------- items

enum class ValueFmt { Percent, TimesInt, Times2, Dec2, Millis };

struct SliderSpec {
    const char* key;
    const wchar_t* name;
    double min, max, def;
    ValueFmt fmt;
    const char* icon;
    double step;        // the Settings slider's step (ui/src/settings-schema.js)
};

inline const SliderSpec* SliderSpecs(int* count) {
    // Ranges and defaults mirror ui/src/settings-schema.js; the ini is the source of truth.
    static const SliderSpec k[] = {
        { "colorWarmPct",   L"Warmth",         0,    100,  0,    ValueFmt::Percent,  "warm", 5 },
        { "colorDimPct",    L"Brightness",     1,    100,  100,  ValueFmt::Percent,  "bright", 1 },
        { "maxLevel",       L"Max zoom",       2,    50,   12,   ValueFmt::TimesInt, "maxz", 1 },
        { "zoomInSpeed",    L"Zoom-in speed",  0.25, 4,    1,    ValueFmt::Times2,   "zin", 0.05 },
        { "zoomOutSpeed",   L"Zoom-out speed", 0.25, 4,    1,    ValueFmt::Times2,   "zout", 0.05 },
        { "panSpeed",       L"Pan speed",      0.25, 4,    1,    ValueFmt::Times2,   "pan", 0.05 },
        { "cursorSmoothing",L"Pan smoothing",  0,    0.95, 0.4,  ValueFmt::Dec2,     "smooth", 0.05 },
        { "zoomEaseOutMs",  L"Release glide",  0,    300,  45,   ValueFmt::Millis,   "glide", 5 },
    };
    if (count) *count = (int)(sizeof(k) / sizeof(k[0]));
    return k;
}

inline const SliderSpec* FindSliderSpec(const std::string& key) {
    int n = 0;
    const SliderSpec* k = SliderSpecs(&n);
    for (int i = 0; i < n; ++i) if (key == k[i].key) return &k[i];
    return nullptr;
}

struct ToggleSpec { const char* key; const wchar_t* name; const char* icon; };

// The "engine" item is the main-engine dropdown under the group (#315); it is never a toggle segment.
inline bool IsEngineKey(const std::string& key) { return key == "engine"; }

inline const ToggleSpec* FindToggleSpec(const std::string& key) {
    static const ToggleSpec k[] = {
        { "trackCaret", L"Follow the text cursor",  "ftc" },
        { "trackFocus", L"Follow keyboard focus",   "ffk" },
        { "keepEdges",  L"Keep within the edges",   "edges" },
        { "engine",     L"Magnifier engine",        "engine" },
    };
    for (const auto& t : k) if (key == t.key) return &t;
    return nullptr;
}

inline bool ParseDouble(const IniValues& v, const char* key, double& out) {
    auto it = v.find(key);
    if (it == v.end()) return false;
    const char* s = it->second.c_str();
    char* e = nullptr;
    const double d = std::strtod(s, &e);
    if (e == s) return false;
    out = d;
    return true;
}

inline double SliderValue(const SliderSpec& s, const IniValues& ini) {
    double v = s.def;
    ParseDouble(ini, s.key, v);
    return (std::min)((std::max)(v, s.min), s.max);
}

inline double SliderFraction(const SliderSpec& s, double v) {
    if (s.max <= s.min) return 0.0;
    return (std::min)((std::max)((v - s.min) / (s.max - s.min), 0.0), 1.0);
}

inline std::wstring Widen(const char* s) {
    std::wstring w;
    for (; s && *s; ++s) w.push_back((wchar_t)(unsigned char)*s);
    return w;
}

// "40%", "12x", "1.00x", "0.40", "45 ms".
inline std::wstring FormatSliderValue(const SliderSpec& s, double v) {
    char b[32];
    switch (s.fmt) {
        case ValueFmt::Percent:  std::snprintf(b, sizeof b, "%d%%", (int)std::lround(v)); break;
        case ValueFmt::TimesInt: std::snprintf(b, sizeof b, "%dx", (int)std::lround(v)); break;
        case ValueFmt::Times2:   std::snprintf(b, sizeof b, "%.2fx", v); break;
        case ValueFmt::Dec2:     std::snprintf(b, sizeof b, "%.2f", v); break;
        case ValueFmt::Millis:   std::snprintf(b, sizeof b, "%d ms", (int)std::lround(v)); break;
    }
    return Widen(b);
}

inline int IniInt(const IniValues& v, const char* key, int def) {
    double d = 0;
    return ParseDouble(v, key, d) ? (int)d : def;
}

// "Keep within the edges" is ONE chip for two settings: it reads ON only when BOTH mouseAlign and
// trackAlign are 1, so a hand-edited mixed state reads OFF (and a click then sets both).
inline bool ToggleOn(const std::string& key, const IniValues& ini) {
    if (key == "keepEdges") return IniInt(ini, "mouseAlign", 0) == 1 && IniInt(ini, "trackAlign", 0) == 1;
    if (key == "trackCaret") return IniInt(ini, "trackCaret", 1) != 0;
    if (key == "trackFocus") return IniInt(ini, "trackFocus", 0) != 0;
    return false;
}

// ---------------------------------------------------------------- performance readout

// Frame-pacing trace for the header sparkline: 0 (bottom) .. 1 (top), the scale being 2x the
// median so a healthy trace is a flat line at 0.5 and a stall spikes above it. Anything under
// 1.5x the median collapses to the median (wake jitter is not a missed frame; see the note in
// tick_stats.h). Down-sampled to at most `maxPts` by bucket maximum so a stall is never averaged
// away.
inline std::vector<float> SparkNorm(const float* ms, int n, int maxPts) {
    std::vector<float> out;
    if (!ms || n < 8 || maxPts < 2) return out;
    const double med = MedianMs(ms, n);
    const double top = med > 0.01 ? med * 2.0 : 20.0;
    const int m = (std::min)(n, maxPts);
    for (int i = 0; i < m; ++i) {
        const int a = (int)((long long)i * n / m), b = (std::max)(a + 1, (int)((long long)(i + 1) * n / m));
        float v = 0.f;
        for (int j = a; j < b && j < n; ++j) {
            const double raw = (ms[j] < med * 1.5) ? med : ms[j];
            v = (std::max)(v, (float)(std::min)(raw / top, 1.0));
        }
        out.push_back(v);
    }
    return out;
}

// ---------------------------------------------------------------- view model

struct SliderView {
    std::string key, icon;
    std::wstring name, text;
    double frac = 0.0;
};
struct ToggleView {
    std::string key, icon;
    std::wstring name;
    bool on = false;
};
struct EngineView {
    std::string icon;
    std::wstring name;
    std::wstring value;         // the current choice ("Transform")
    bool open = false;          // the engine list is open under the dropdown
};
struct PerfView {
    bool zoomed = false;
    std::wstring zoom = L"Idle";    // "7.4x" or "Idle"
    bool haveFps = false;
    int fps = 0;
    std::wstring frameMs;           // "6.9 ms"
    std::vector<float> spark;       // see SparkNorm; empty = flat dashed rule
};
// Animation amounts (0..1) for the drawn state, owned by the window. `active` false = the painter
// uses the plain hover/on state (render-test, unit tests), so a still frame is identical either way.
struct AnimView {
    bool active = false;
    std::vector<float> chipHot, chipOn;   // per toggle segment
    float engHot = 0, engPress = 0;                  // the engine dropdown
    float btnHot[3] = { 0, 0, 0 }, btnPress[3] = { 0, 0, 0 };   // profile, settings, quit
};

// Moves `cur` toward `target` linearly so a full 0..1 swing takes `durMs`. PURE.
inline float StepToward(float cur, float target, float dtMs, float durMs) {
    if (durMs <= 0.f || dtMs >= durMs) return target;
    const float step = dtMs / durMs;
    if (cur < target) return cur + step > target ? target : cur + step;
    return cur - step < target ? target : cur - step;
}

struct View {
    bool dark = true;
    bool perf = false;
    PerfView p;
    std::vector<SliderView> sliders;
    std::vector<ToggleView> toggles;        // the segments of the toggle group, in order
    bool hasEngine = false;                 // the dropdown row under the group
    EngineView engine;
    std::wstring profile = L"Default";
    Hit hover;
    Hit focus;                      // keyboard focus; drawn only when showFocus
    bool showFocus = false;
    AnimView anim;
};

inline Geometry ComputeGeometry(const View& v, int profileTextW) {
    return ComputeGeometry(v.perf, (int)v.sliders.size(), (int)v.toggles.size(), v.hasEngine, profileTextW);
}

inline PerfView BuildPerf(const TrayStatus& st, const float* ticks, int n) {
    PerfView p;
    wchar_t z[16];
    p.zoomed = FormatZoom(st.level, z, 16);
    p.zoom = p.zoomed ? std::wstring(z) + L"x" : std::wstring(L"Idle");
    if (ticks && n >= 8) {
        const double mean = MeanMs(ticks, n);
        p.haveFps = true;
        p.fps = (int)(FpsFromMs(mean) + 0.5);
        char b[32];
        std::snprintf(b, sizeof b, "%.1f ms", mean);
        p.frameMs = Widen(b);
        p.spark = SparkNorm(ticks, n, 64);
    } else {
        p.frameMs = L"– ms";
    }
    return p;
}

// Items the layout enables, in its order. A key that is not a known item is skipped (an older or
// hand-edited ini); ParseTrayLayout has already dropped unknown keys and applied the slider cap.
inline View BuildView(const IniValues& ini, const TrayLayout& layout, const TrayStatus& st,
                      const float* ticks, int nTicks, const std::wstring& profile, bool dark) {
    View v;
    v.dark = dark;
    v.perf = layout.perf;
    if (layout.perf) v.p = BuildPerf(st, ticks, nTicks);
    for (const auto& it : layout.sliders) {
        if (!it.on) continue;
        const SliderSpec* s = FindSliderSpec(it.key);
        if (!s) continue;
        SliderView sv;
        sv.key = s->key; sv.icon = s->icon; sv.name = s->name;
        const double val = SliderValue(*s, ini);
        sv.frac = SliderFraction(*s, val);
        sv.text = FormatSliderValue(*s, val);
        v.sliders.push_back(sv);
    }
    for (const auto& it : layout.toggles) {
        if (!it.on) continue;
        const ToggleSpec* t = FindToggleSpec(it.key);
        if (!t) continue;
        if (IsEngineKey(it.key)) {
            auto mi = ini.find(kEngineKey);
            v.hasEngine = true;
            v.engine.icon = t->icon; v.engine.name = t->name;
            v.engine.value = EngineLabel(EngineIndex(mi == ini.end() ? std::string() : mi->second));
            continue;
        }
        ToggleView tv;
        tv.key = t->key; tv.icon = t->icon; tv.name = t->name;
        tv.on = ToggleOn(it.key, ini);
        v.toggles.push_back(tv);
    }
    v.profile = profile.empty() ? std::wstring(L"Default") : profile;
    return v;
}

// ---------------------------------------------------------------- interaction (pure)

// A slider value snapped to the Settings slider's step (from `min`) and clamped to its range.
inline double SnapSlider(const SliderSpec& s, double v) {
    v = (std::min)((std::max)(v, s.min), s.max);
    if (s.step > 0) {
        v = s.min + std::floor((v - s.min) / s.step + 0.5) * s.step;
        v = (std::min)((std::max)(v, s.min), s.max);
    }
    return v;
}

// The value a pointer at `x` (DIPs) means on `track`; clamps beyond either end.
inline double SliderFromX(const SliderSpec& s, const IRect& track, int x) {
    if (track.w() <= 0) return s.def;
    const double f = (std::min)((std::max)((double)(x - track.l) / (double)track.w(), 0.0), 1.0);
    return SnapSlider(s, s.min + f * (s.max - s.min));
}

// One keyboard step up (dir > 0) or down; `big` (Shift) moves about a tenth of the range.
inline double StepSlider(const SliderSpec& s, double cur, int dir, bool big) {
    double d = s.step > 0 ? s.step : (s.max - s.min) / 100.0;
    if (big) d = (std::max)(d, std::floor((s.max - s.min) * 0.1 / d + 0.5) * d);
    return SnapSlider(s, cur + (dir > 0 ? d : -d));
}

// The ini text for a slider value: whole numbers for %, x-int and ms; two decimals otherwise.
inline std::string FormatIniValue(const SliderSpec& s, double v) {
    char b[32];
    switch (s.fmt) {
        case ValueFmt::Percent: case ValueFmt::TimesInt: case ValueFmt::Millis:
            std::snprintf(b, sizeof b, "%d", (int)std::lround(v)); break;
        default: std::snprintf(b, sizeof b, "%.2f", v); break;
    }
    return b;
}

// One ini write. Toggling "Keep within the edges" is two of them (mouseAlign and trackAlign).
struct IniChange { std::string key, value; };

inline std::vector<IniChange> ToggleChanges(const std::string& key, bool turnOn) {
    const char* v = turnOn ? "1" : "0";
    if (key == "keepEdges") return { { "mouseAlign", v }, { "trackAlign", v } };
    return { { key, v } };
}

inline void ApplyChanges(IniValues& ini, const std::vector<IniChange>& ch) {
    for (const auto& c : ch) ini[c.key] = c.value;
}

// Writes while dragging a slider: at most one per kMinMs, the rest coalesced; whatever is still
// pending is flushed by a timer (and unconditionally on release), so the final value always lands.
struct WriteThrottle {
    static constexpr unsigned long long kMinMs = 50;
    unsigned long long lastMs = 0;
    bool due(unsigned long long nowMs) const { return lastMs == 0 || nowMs < lastMs || nowMs - lastMs >= kMinMs; }
    unsigned long long waitMs(unsigned long long nowMs) const {
        return due(nowMs) ? 0 : kMinMs - (nowMs - lastMs);
    }
    void wrote(unsigned long long nowMs) { lastMs = nowMs; }
};

// A press on a slider row only starts a drag on (or just beside) the track, not on its icon or value.
inline bool SliderTrackHit(const Geometry& g, int i, int x, int y) {
    if (i < 0 || i >= (int)g.sliderTrack.size()) return false;
    const IRect& t = g.sliderTrack[i];
    return g.sliderRow[i].contains(x, y) && x >= t.l - 8 && x < t.r + 8;
}

// Keyboard focus walks the controls in reading order: sliders, toggle segments, the engine dropdown,
// profile, Settings, Quit.
inline int FocusCount(const Geometry& g) {
    return (int)g.sliderRow.size() + (int)g.seg.size() + (g.engine.w() > 0 ? 1 : 0) + 3;
}
inline Hit FocusHit(const Geometry& g, int idx) {
    const int ns = (int)g.sliderRow.size(), nt = (int)g.seg.size(), ne = g.engine.w() > 0 ? 1 : 0;
    if (idx < 0) return {};
    if (idx < ns) return { HitKind::Slider, idx };
    if (idx < ns + nt) return { HitKind::Toggle, idx - ns };
    if (idx < ns + nt + ne) return { HitKind::Engine, 0 };
    switch (idx - ns - nt - ne) {
        case 0: return { HitKind::Profile, 0 };
        case 1: return { HitKind::Settings, 0 };
        case 2: return { HitKind::Quit, 0 };
    }
    return {};
}
inline int FocusIndex(const Geometry& g, const Hit& h) {
    const int ns = (int)g.sliderRow.size(), nt = (int)g.seg.size(), ne = g.engine.w() > 0 ? 1 : 0;
    switch (h.kind) {
        case HitKind::Slider:   return h.index;
        case HitKind::Toggle:   return ns + h.index;
        case HitKind::Engine:   return ne ? ns + nt : -1;
        case HitKind::Profile:  return ns + nt + ne;
        case HitKind::Settings: return ns + nt + ne + 1;
        case HitKind::Quit:     return ns + nt + ne + 2;
        default: return -1;
    }
}
// Tab (back = false) or Shift+Tab from `cur` (-1 = nothing focused yet), wrapping.
inline int NextFocus(int cur, int count, bool back) {
    if (count <= 0) return -1;
    if (cur < 0) return back ? count - 1 : 0;
    return back ? (cur + count - 1) % count : (cur + 1) % count;
}
// Left / Right inside the toggle group: one segment over, stopping at the ends (no wrap).
inline int SegNeighbor(const Geometry& g, int idx, int dx) {
    const int n = (int)g.seg.size();
    if (idx < 0 || idx >= n) return idx;
    return (std::min)((std::max)(idx + (dx > 0 ? 1 : -1), 0), n - 1);
}

inline IRect FocusRect(const Geometry& g, const Hit& h) {
    switch (h.kind) {
        case HitKind::Slider:
            if (h.index >= 0 && h.index < (int)g.sliderRow.size()) {
                const IRect& r = g.sliderRow[h.index];
                return { r.l + 8, r.t + 2, r.r - 8, r.b - 2 };
            }
            break;
        case HitKind::Toggle:   if (h.index >= 0 && h.index < (int)g.seg.size()) return g.seg[h.index]; break;
        case HitKind::Engine:   return g.engine;
        case HitKind::Profile:  return g.profileBtn;
        case HitKind::Settings: return g.settingsBtn;
        case HitKind::Quit:     return g.quitBtn;
        default: break;
    }
    return {};
}

// Up / Down from a control that is not a slider (Up/Down adjust a slider; Tab leaves it): the rows
// are the toggle group, the engine dropdown and the bottom row. The target is the control of the
// next present row, up or down, whose centre is nearest to the current one. Stays put at the
// bottom; from the group (or the dropdown when there is no group) Up goes to the last slider when
// there is one, else stays.
inline Hit VerticalNeighbor(const Geometry& g, const Hit& cur, int dir) {
    const bool hasGroup = !g.seg.empty(), hasEng = g.engine.w() > 0;
    const Hit lastSlider = g.sliderRow.empty() ? cur : Hit{ HitKind::Slider, (int)g.sliderRow.size() - 1 };
    const IRect me = FocusRect(g, cur);
    auto nearestOf = [&](const std::vector<Hit>& c) {
        Hit best = c.front();
        int bd = 1 << 30;
        const int cx = (me.l + me.r) / 2;
        for (const Hit& h : c) {
            const IRect r = FocusRect(g, h);
            const int d = std::abs((r.l + r.r) / 2 - cx);
            if (d < bd) { bd = d; best = h; }
        }
        return best;
    };
    std::vector<Hit> group;
    for (size_t i = 0; i < g.seg.size(); ++i) group.push_back({ HitKind::Toggle, (int)i });
    const std::vector<Hit> bottom = { { HitKind::Profile, 0 }, { HitKind::Settings, 0 }, { HitKind::Quit, 0 } };
    switch (cur.kind) {
        case HitKind::Toggle:
            if (dir > 0) return hasEng ? Hit{ HitKind::Engine, 0 } : nearestOf(bottom);
            return lastSlider;
        case HitKind::Engine:
            if (dir > 0) return nearestOf(bottom);
            return hasGroup ? nearestOf(group) : lastSlider;
        case HitKind::Profile: case HitKind::Settings: case HitKind::Quit:
            if (dir > 0) return cur;
            if (hasEng) return { HitKind::Engine, 0 };
            return hasGroup ? nearestOf(group) : lastSlider;
        default: return cur;
    }
}

// ---------------------------------------------------------------- profile list popup (pure)

inline constexpr int kListRowH = 32, kListPad = 4, kListMinW = 140, kListMaxW = 296, kListTextPad = 12, kListCheckW = 28;

inline constexpr int kListCaptionH = 28;      // the caption line above the rows (the engine list)

struct ListGeometry {
    int width = 0, height = 0;
    IRect caption;                  // empty (w() == 0) when the list has no caption
    std::vector<IRect> row;
};

// `captionW` > 0 adds a caption line above the rows and widens the list to fit it. `fixedW` > 0 pins
// the width (the engine list is exactly as wide as its dropdown, mockup v02) and ignores the text.
inline ListGeometry ComputeList(int n, int widestTextW, int captionW = 0, int fixedW = 0) {
    ListGeometry g;
    g.width = fixedW > 0 ? fixedW
        : ClampInt((std::max)(widestTextW + kListCheckW, captionW) + 2 * kListTextPad + 2 * kListPad + 2 * kBorder,
                   kListMinW, kListMaxW);
    int y = kBorder + kListPad;
    if (captionW > 0) {
        g.caption = { kBorder + kListPad, y, g.width - kBorder - kListPad, y + kListCaptionH };
        y += kListCaptionH;
    }
    for (int i = 0; i < n; ++i) {
        g.row.push_back({ kBorder + kListPad, y, g.width - kBorder - kListPad, y + kListRowH });
        y += kListRowH;
    }
    g.height = y + kListPad + kBorder;
    return g;
}

inline int ListHitTest(const ListGeometry& g, int x, int y) {
    for (size_t i = 0; i < g.row.size(); ++i) if (g.row[i].contains(x, y)) return (int)i;
    return -1;
}

// Up/Down in the list, clamped (no wrap); -1 (nothing selected yet) starts at the first or last.
inline int ListStep(int cur, int n, int dir) {
    if (n <= 0) return -1;
    if (cur < 0) return dir > 0 ? 0 : n - 1;
    return (std::min)((std::max)(cur + (dir > 0 ? 1 : -1), 0), n - 1);
}

// Top-left (pixels) of a w x h list opening ABOVE the anchor button, left-aligned with it; below
// when there is no room above (taskbar on top). Clamped to the work area.
inline Placement PlaceList(const IRect& anchor, const IRect& work, int w, int h, int gap, bool preferBelow = false) {
    Placement p;
    p.x = ClampInt(anchor.l, work.l + gap, work.r - w - gap);
    int y = anchor.t - gap - h;
    if (preferBelow) {                       // a dropdown opens under its field, and flips up when it cannot
        const int below = anchor.b + gap;
        y = below + h <= work.b - gap ? below : y;
        if (y < work.t + gap) y = below;
    } else if (y < work.t + gap) {
        y = anchor.b + gap;
    }
    p.y = ClampInt(y, work.t + gap, work.b - h - gap);
    return p;
}

struct ListView {
    bool dark = true;
    std::vector<std::wstring> names;
    int active = -1;      // the current profile (checkmark)
    int sel = -1;         // hover or keyboard selection
    std::wstring caption; // a line above the rows (the engine list); empty = none
};

}}  // namespace wind::Flyout
