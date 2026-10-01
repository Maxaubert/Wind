// The tray flyout's painter (see flyout_draw.h). Everything is laid out in DIPs from flyout_model.h;
// colours and metrics are the j01-calm-tint tokens.
#include "flyout_draw.h"
#include "flyout_icons.h"
#include "svg_path.h"
#include "../resource.h"
#include <d2d1helper.h>
#include <dwrite_1.h>
#include <wincodec.h>
#include <shlwapi.h>
#include <wrl/client.h>
#include <algorithm>
#include <map>
#include <vector>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "shlwapi.lib")

using Microsoft::WRL::ComPtr;

namespace wind { namespace Flyout {

// ---------------------------------------------------------------- shared state

namespace {

struct Shared {
    bool ok = false;
    ComPtr<ID2D1Factory> d2d;
    ComPtr<IDWriteFactory> dw;
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IDWriteTextFormat> sans28, mono12, mono12b;
    std::vector<BYTE> auroraDark, auroraLight;   // premultiplied BGRA, 1920 x 300
    UINT aw = 0, ah = 0;
    std::map<std::string, ComPtr<ID2D1PathGeometry>> icons;
};
Shared g_s;

D2D1_COLOR_F Col(unsigned rgb, float a = 1.f) {
    return D2D1::ColorF(((rgb >> 16) & 0xff) / 255.f, ((rgb >> 8) & 0xff) / 255.f, (rgb & 0xff) / 255.f, a);
}

struct Theme {
    D2D1_COLOR_F menu, menub, fg, fg2, fg3, rule, hl, glyph, teal, lift, scrim, chipb, band, track;
    D2D1_COLOR_F off, offh, offic, onic, on, onh, onb;
    float aurora;       // --ac
    bool dark;
};

Theme MakeTheme(bool dark) {
    Theme t;
    t.dark = dark;
    if (dark) {
        t.menu = Col(0x000000); t.menub = Col(0x333333); t.fg = Col(0xf2f2f2); t.fg2 = Col(0xd0d0d0);
        t.fg3 = Col(0xb4b6ba); t.rule = Col(0x303236); t.hl = Col(0x2d2d2d); t.glyph = Col(0xb0b0b0);
        t.teal = Col(0x2fbfa5); t.lift = Col(0x0b0b0b); t.scrim = Col(0x000000, .55f);
        t.chipb = Col(0x3a3a3a); t.band = Col(0x0a0a0a); t.track = Col(0x3d3d3d);
        t.off = Col(0x303033); t.offh = Col(0x3b3b3f); t.offic = Col(0xc8cad0); t.onic = Col(0xa9ece0);
        t.on = Col(0x1f5650); t.onh = Col(0x266560); t.onb = Col(0x2fbfa5, .4f);
        t.aurora = .62f;
    } else {
        t.menu = Col(0xffffff); t.menub = Col(0xd9d9d9); t.fg = Col(0x0a0a0a); t.fg2 = Col(0x2e2e2e);
        t.fg3 = Col(0x45484d); t.rule = Col(0xc6c9ce); t.hl = Col(0xececec); t.glyph = Col(0x555555);
        t.teal = Col(0x087a67); t.lift = Col(0xf6f7f8); t.scrim = Col(0xffffff, .78f);
        t.chipb = Col(0xcdcdcd); t.band = Col(0xf1f2f4); t.track = Col(0xd0d3d6);
        t.off = Col(0xe5e6e8); t.offh = Col(0xdadbde); t.offic = Col(0x3d4147); t.onic = Col(0x0a5a4d);
        t.on = Col(0xbfe2db); t.onh = Col(0xb2d9d1); t.onb = Col(0x087a67, .4f);
        t.aurora = .85f;
    }
    return t;
}

// The light theme inverts the aurora with the CSS filter the mockup uses:
// invert(1) hue-rotate(180deg) saturate(1.6) contrast(1.25), each stage clamped like a browser.
void LightenAurora(std::vector<BYTE>& px) {
    auto clamp01 = [](float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); };
    const float s = 1.6f;
    for (size_t i = 0; i + 3 < px.size(); i += 4) {
        float b = px[i] / 255.f, g = px[i + 1] / 255.f, r = px[i + 2] / 255.f;
        r = 1.f - r; g = 1.f - g; b = 1.f - b;
        float r2 = clamp01(-0.574f * r + 1.430f * g + 0.144f * b);
        float g2 = clamp01(0.426f * r + 0.430f * g + 0.144f * b);
        float b2 = clamp01(0.426f * r + 1.430f * g - 0.856f * b);
        r = clamp01((0.213f + 0.787f * s) * r2 + (0.715f - 0.715f * s) * g2 + (0.072f - 0.072f * s) * b2);
        g = clamp01((0.213f - 0.213f * s) * r2 + (0.715f + 0.285f * s) * g2 + (0.072f - 0.072f * s) * b2);
        b = clamp01((0.213f - 0.213f * s) * r2 + (0.715f - 0.715f * s) * g2 + (0.072f + 0.928f * s) * b2);
        r = clamp01((r - .5f) * 1.25f + .5f); g = clamp01((g - .5f) * 1.25f + .5f); b = clamp01((b - .5f) * 1.25f + .5f);
        px[i] = (BYTE)(b * 255.f + .5f); px[i + 1] = (BYTE)(g * 255.f + .5f); px[i + 2] = (BYTE)(r * 255.f + .5f);
    }
}

bool HasFamily(const wchar_t* name) {
    ComPtr<IDWriteFontCollection> col;
    if (FAILED(g_s.dw->GetSystemFontCollection(&col, FALSE))) return false;
    UINT32 idx = 0; BOOL ex = FALSE;
    return SUCCEEDED(col->FindFamilyName(name, &idx, &ex)) && ex;
}

const wchar_t* FirstFamily(std::initializer_list<const wchar_t*> names) {
    for (const wchar_t* n : names) if (HasFamily(n)) return n;
    return *names.begin();
}

bool MakeFormat(const wchar_t* family, float size, DWRITE_FONT_WEIGHT w, ComPtr<IDWriteTextFormat>& out) {
    return SUCCEEDED(g_s.dw->CreateTextFormat(family, nullptr, w, DWRITE_FONT_STYLE_NORMAL,
                                              DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &out));
}

void LoadAurora() {
    HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(IDR_AURORA), RT_RCDATA);
    if (!r) return;
    HGLOBAL h = LoadResource(nullptr, r);
    const void* p = h ? LockResource(h) : nullptr;
    const DWORD n = SizeofResource(nullptr, r);
    if (!p || !n) return;
    ComPtr<IStream> st;
    st.Attach(SHCreateMemStream(static_cast<const BYTE*>(p), n));
    if (!st) return;
    ComPtr<IWICBitmapDecoder> dec;
    if (FAILED(g_s.wic->CreateDecoderFromStream(st.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &dec))) return;
    ComPtr<IWICBitmapFrameDecode> fr;
    if (FAILED(dec->GetFrame(0, &fr))) return;
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(g_s.wic->CreateFormatConverter(&conv))) return;
    if (FAILED(conv->Initialize(fr.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr,
                                0.0, WICBitmapPaletteTypeCustom))) return;
    UINT w = 0, hgt = 0;
    conv->GetSize(&w, &hgt);
    if (!w || !hgt) return;
    g_s.auroraDark.resize((size_t)w * hgt * 4);
    if (FAILED(conv->CopyPixels(nullptr, w * 4, (UINT)g_s.auroraDark.size(), g_s.auroraDark.data()))) {
        g_s.auroraDark.clear();
        return;
    }
    g_s.aw = w; g_s.ah = hgt;
    g_s.auroraLight = g_s.auroraDark;
    LightenAurora(g_s.auroraLight);
}

ID2D1PathGeometry* IconGeometry(const std::string& id) {
    auto it = g_s.icons.find(id);
    if (it != g_s.icons.end()) return it->second.Get();
    const char* d = IconPath(id.c_str());
    if (!d) return nullptr;
    ComPtr<ID2D1PathGeometry> pg;
    if (FAILED(g_s.d2d->CreatePathGeometry(&pg))) return nullptr;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(pg->Open(&sink))) return nullptr;
    bool open = false;
    for (const PathSeg& s : ParseSvgPath(d)) {
        switch (s.op) {
            case 'M':
                if (open) sink->EndFigure(D2D1_FIGURE_END_OPEN);
                sink->BeginFigure(D2D1::Point2F(s.v[0], s.v[1]), D2D1_FIGURE_BEGIN_HOLLOW);
                open = true;
                break;
            case 'L': if (open) sink->AddLine(D2D1::Point2F(s.v[0], s.v[1])); break;
            case 'C':
                if (open) sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(s.v[0], s.v[1]),
                                                              D2D1::Point2F(s.v[2], s.v[3]),
                                                              D2D1::Point2F(s.v[4], s.v[5])));
                break;
            case 'A':
                if (open) sink->AddArc(D2D1::ArcSegment(
                    D2D1::Point2F(s.v[5], s.v[6]), D2D1::SizeF(s.v[0], s.v[1]), s.v[2],
                    s.v[4] != 0.f ? D2D1_SWEEP_DIRECTION_CLOCKWISE : D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE,
                    s.v[3] != 0.f ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
                break;
            case 'Z':
                if (open) { sink->EndFigure(D2D1_FIGURE_END_CLOSED); open = false; }
                break;
        }
    }
    if (open) sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->Close();
    ID2D1PathGeometry* raw = pg.Get();
    g_s.icons[id] = pg;
    return raw;
}

}  // namespace

bool DrawInit() {
    if (g_s.ok) return true;
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, g_s.d2d.GetAddressOf()))) return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(g_s.dw.GetAddressOf())))) return false;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&g_s.wic)))) return false;
    const wchar_t* sans = FirstFamily({ L"Segoe UI Variable Display", L"Segoe UI Variable", L"Segoe UI" });
    const wchar_t* mono = FirstFamily({ L"Cascadia Mono", L"Consolas", L"Courier New" });
    if (!MakeFormat(sans, 28.f, DWRITE_FONT_WEIGHT_MEDIUM, g_s.sans28)) return false;
    if (!MakeFormat(mono, 12.f, DWRITE_FONT_WEIGHT_NORMAL, g_s.mono12)) return false;
    if (!MakeFormat(mono, 12.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, g_s.mono12b)) return false;
    LoadAurora();
    g_s.ok = true;
    return true;
}

void DrawShutdown() {
    g_s.icons.clear();
    g_s.sans28.Reset(); g_s.mono12.Reset(); g_s.mono12b.Reset();
    g_s.wic.Reset(); g_s.dw.Reset(); g_s.d2d.Reset();
    g_s.auroraDark.clear(); g_s.auroraLight.clear();
    g_s.ok = false;
}

int MeasureProfileText(const std::wstring& name) {
    if (!g_s.ok || name.empty()) return 0;
    ComPtr<IDWriteTextLayout> lay;
    if (FAILED(g_s.dw->CreateTextLayout(name.c_str(), (UINT32)name.size(), g_s.mono12.Get(), 1000.f, 20.f, &lay)))
        return 0;
    lay->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    DWRITE_TEXT_METRICS m{};
    lay->GetMetrics(&m);
    return (int)(m.widthIncludingTrailingWhitespace + .999f);
}

ID2D1DCRenderTarget* CreateDcTarget(int dpi) {
    if (!g_s.ok) return nullptr;
    D2D1_RENDER_TARGET_PROPERTIES p = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        (float)dpi, (float)dpi, D2D1_RENDER_TARGET_USAGE_NONE, D2D1_FEATURE_LEVEL_DEFAULT);
    ID2D1DCRenderTarget* rt = nullptr;
    if (FAILED(g_s.d2d->CreateDCRenderTarget(&p, &rt))) return nullptr;
    return rt;
}

// ---------------------------------------------------------------- painter

struct Painter::Impl {
    ComPtr<ID2D1RenderTarget> rt;
    ComPtr<ID2D1SolidColorBrush> br;
    ComPtr<ID2D1StrokeStyle> round, dotted;
    ComPtr<ID2D1Bitmap> aurora;
    Theme th;

    void fill(const D2D1_RECT_F& r, const D2D1_COLOR_F& c) { br->SetColor(c); rt->FillRectangle(r, br.Get()); }
    void fillRound(const D2D1_RECT_F& r, float rad, const D2D1_COLOR_F& c) {
        br->SetColor(c);
        rt->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), br.Get());
    }
    void ring(const D2D1_RECT_F& r, float rad, const D2D1_COLOR_F& c) {   // 1 px inset ring
        br->SetColor(c);
        rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left + .5f, r.top + .5f, r.right - .5f, r.bottom - .5f),
                                                   rad - .5f, rad - .5f), br.Get(), 1.f);
    }
    D2D1_RECT_F R(const IRect& r) const { return D2D1::RectF((float)r.l, (float)r.t, (float)r.r, (float)r.b); }

    void icon(const std::string& id, float x, float y, const D2D1_COLOR_F& c, float w) {
        ID2D1PathGeometry* g = IconGeometry(id);
        if (!g) return;
        rt->SetTransform(D2D1::Matrix3x2F::Translation(x, y));
        br->SetColor(c);
        rt->DrawGeometry(g, br.Get(), w, round.Get());
        rt->SetTransform(D2D1::Matrix3x2F::Identity());
    }

    // Text in a box, vertically centred. `spacing` is the trailing letter spacing in DIPs.
    float text(const std::wstring& s, IDWriteTextFormat* f, const D2D1_RECT_F& box, const D2D1_COLOR_F& c,
               DWRITE_TEXT_ALIGNMENT ha, float spacing = 0.f) {
        if (s.empty()) return 0.f;
        ComPtr<IDWriteTextLayout> lay;
        if (FAILED(g_s.dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), f, box.right - box.left,
                                            box.bottom - box.top, &lay))) return 0.f;
        lay->SetTextAlignment(ha);
        lay->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        lay->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        if (spacing != 0.f) {
            ComPtr<IDWriteTextLayout1> l1;
            if (SUCCEEDED(lay.As(&l1))) l1->SetCharacterSpacing(0.f, spacing, 0.f, DWRITE_TEXT_RANGE{ 0, (UINT32)s.size() });
        }
        DWRITE_TEXT_METRICS m{};
        lay->GetMetrics(&m);
        br->SetColor(c);
        rt->DrawTextLayout(D2D1::Point2F(box.left, box.top), lay.Get(), br.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
        return m.widthIncludingTrailingWhitespace;
    }
    float measure(const std::wstring& s, IDWriteTextFormat* f) {
        ComPtr<IDWriteTextLayout> lay;
        if (s.empty() || FAILED(g_s.dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), f, 1000.f, 20.f, &lay))) return 0.f;
        lay->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        DWRITE_TEXT_METRICS m{};
        lay->GetMetrics(&m);
        return m.widthIncludingTrailingWhitespace;
    }

    ComPtr<ID2D1LinearGradientBrush> gradient(D2D1_POINT_2F a, D2D1_POINT_2F b,
                                              std::initializer_list<D2D1_GRADIENT_STOP> stops) {
        std::vector<D2D1_GRADIENT_STOP> v(stops);
        ComPtr<ID2D1GradientStopCollection> col;
        ComPtr<ID2D1LinearGradientBrush> out;
        if (SUCCEEDED(rt->CreateGradientStopCollection(v.data(), (UINT32)v.size(), D2D1_GAMMA_2_2,
                                                       D2D1_EXTEND_MODE_CLAMP, &col)))
            rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(a, b), col.Get(), &out);
        return out;
    }

    void drawHead(const View& v, const Geometry& g);
    void drawQuick(const View& v, const Geometry& g);
    void drawBar(const View& v, const Geometry& g);
};

static D2D1_COLOR_F WithAlpha(D2D1_COLOR_F c, float a) { c.a = a; return c; }

void Painter::Impl::drawHead(const View& v, const Geometry& g) {
    const D2D1_RECT_F h = R(g.head);
    const float W = h.right - h.left, H = h.bottom - h.top;
    fill(h, th.band);
    if (aurora) {
        // background: right center / cover
        const float sc = (std::max)(W / (float)g_s.aw, H / (float)g_s.ah);
        const float sw = W / sc, sh = H / sc;
        const D2D1_RECT_F src = D2D1::RectF((float)g_s.aw - sw, ((float)g_s.ah - sh) / 2.f, (float)g_s.aw,
                                            ((float)g_s.ah + sh) / 2.f);
        rt->DrawBitmap(aurora.Get(), h, th.aurora, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, src);
        // mask-image: linear-gradient(90deg, transparent 15%, #000 70%) == the band colour fades the
        // image back out on the left.
        auto m = gradient(D2D1::Point2F(h.left, 0), D2D1::Point2F(h.right, 0),
                          { { 0.f, th.band }, { .15f, th.band }, { .70f, WithAlpha(th.band, 0.f) },
                            { 1.f, WithAlpha(th.band, 0.f) } });
        if (m) rt->FillRectangle(h, m.Get());
    }
    // ::after scrims: 180deg transparent 40% -> scrim, and 90deg scrim -> transparent 85%
    auto s1 = gradient(D2D1::Point2F(0, h.top), D2D1::Point2F(0, h.bottom),
                       { { 0.f, WithAlpha(th.scrim, 0.f) }, { .40f, WithAlpha(th.scrim, 0.f) }, { 1.f, th.scrim } });
    if (s1) rt->FillRectangle(h, s1.Get());
    auto s2 = gradient(D2D1::Point2F(h.left, 0), D2D1::Point2F(h.right, 0),
                       { { 0.f, th.scrim }, { .85f, WithAlpha(th.scrim, 0.f) }, { 1.f, WithAlpha(th.scrim, 0.f) } });
    if (s2) rt->FillRectangle(h, s2.Get());

    const float padX = (float)kPadX, left = h.left + padX, right = h.right - padX;
    const float topRow = h.top + 18.f;
    // zoom level, 28 px, tight tracking
    text(v.p.zoom, g_s.sans28.Get(), D2D1::RectF(left, topRow, right, topRow + 28.f),
         v.p.zoomed ? th.fg : th.fg3, DWRITE_TEXT_ALIGNMENT_LEADING, -0.7f);
    if (v.p.haveFps) {
        wchar_t b[24];
        wsprintfW(b, L"%d fps", v.p.fps);
        text(b, g_s.mono12b.Get(), D2D1::RectF(left, topRow, right, topRow + 28.f), th.fg, DWRITE_TEXT_ALIGNMENT_TRAILING);
    }
    // frame row: "Frame" [sparkline] "6.9 ms"
    const float rowT = topRow + 28.f + 14.f, rowB = rowT + 14.f;
    const float fw = text(L"Frame", g_s.mono12.Get(), D2D1::RectF(left, rowT, right, rowB), th.fg2, DWRITE_TEXT_ALIGNMENT_LEADING);
    const float mw = measure(v.p.frameMs, g_s.mono12b.Get());
    text(v.p.frameMs, g_s.mono12b.Get(), D2D1::RectF(left, rowT, right, rowB), th.fg, DWRITE_TEXT_ALIGNMENT_TRAILING);
    const float sl = left + fw + 10.f, sr = right - mw - 10.f;
    if (sr - sl > 8.f) {
        if (v.p.spark.size() >= 2) {
            ComPtr<ID2D1PathGeometry> pg;
            ComPtr<ID2D1GeometrySink> sink;
            if (SUCCEEDED(g_s.d2d->CreatePathGeometry(&pg)) && SUCCEEDED(pg->Open(&sink))) {
                const size_t n = v.p.spark.size();
                for (size_t i = 0; i < n; ++i) {
                    const D2D1_POINT_2F p = D2D1::Point2F(sl + (sr - sl) * (float)i / (float)(n - 1),
                                                          rowB - 1.f - v.p.spark[i] * (rowB - rowT - 2.f));
                    if (i == 0) sink->BeginFigure(p, D2D1_FIGURE_BEGIN_HOLLOW); else sink->AddLine(p);
                }
                sink->EndFigure(D2D1_FIGURE_END_OPEN);
                sink->Close();
                br->SetColor(th.teal);
                rt->DrawGeometry(pg.Get(), br.Get(), 1.5f, round.Get());
            }
        } else {
            br->SetColor(WithAlpha(th.fg3, .55f));
            const float y = (rowT + rowB) / 2.f;
            rt->DrawLine(D2D1::Point2F(sl, y), D2D1::Point2F(sr, y), br.Get(), 1.f, dotted.Get());
        }
    }
}

void Painter::Impl::drawQuick(const View& v, const Geometry& g) {
    fill(R(g.qs), th.lift);
    for (size_t i = 0; i < v.sliders.size() && i < g.sliderRow.size(); ++i) {
        const SliderView& s = v.sliders[i];
        const IRect& ic = g.sliderIcon[i];
        icon(s.icon, (float)ic.l, (float)ic.t, th.glyph, 1.5f);
        const IRect& tr = g.sliderTrack[i];
        fillRound(R(tr), 3.f, th.track);
        const float x = (float)tr.l + (float)tr.w() * (float)s.frac;
        if (x > (float)tr.l) fillRound(D2D1::RectF((float)tr.l, (float)tr.t, x, (float)tr.b), 3.f, th.teal);
        const float mid = (float)(tr.t + tr.b) / 2.f;
        fillRound(D2D1::RectF(x - 3.f, mid - 5.f, x, mid + 5.f), 1.5f, th.fg);
        text(s.text, g_s.mono12.Get(), R(g.sliderValue[i]), th.fg, DWRITE_TEXT_ALIGNMENT_TRAILING);
    }
    for (size_t i = 0; i < v.toggles.size() && i < g.chip.size(); ++i) {
        const ToggleView& t = v.toggles[i];
        const bool hot = v.hover.kind == HitKind::Chip && v.hover.index == (int)i;
        const D2D1_RECT_F r = R(g.chip[i]);
        if (t.on) {
            fillRound(r, 8.f, hot ? th.onh : th.on);
            ring(r, 8.f, th.onb);
        } else {
            fillRound(r, 8.f, hot ? th.offh : th.off);
            if (hot) ring(r, 8.f, th.chipb);
        }
        const D2D1_COLOR_F ic = t.on ? th.onic : (hot ? th.fg : th.offic);
        icon(t.icon, r.left + 16.f, r.top + 8.f, ic, 1.75f);
    }
}

void Painter::Impl::drawBar(const View& v, const Geometry& g) {
    struct Btn { const IRect* r; HitKind k; const char* icon; };
    const Btn btns[] = { { &g.profileBtn, HitKind::Profile, "profile" },
                         { &g.settingsBtn, HitKind::Settings, "settings" },
                         { &g.quitBtn, HitKind::Quit, "quit" } };
    for (const Btn& b : btns) {
        const bool hot = v.hover.kind == b.k;
        const D2D1_RECT_F r = R(*b.r);
        if (hot) fillRound(r, 8.f, th.hl);
        const float ix = b.k == HitKind::Profile ? r.left + 8.f : r.left + (r.right - r.left - 16.f) / 2.f;
        icon(b.icon, ix, r.top + 8.f, hot ? th.fg : th.glyph, 1.5f);
        if (b.k == HitKind::Profile)
            text(v.profile, g_s.mono12.Get(), D2D1::RectF(r.left + 8.f + 16.f + 12.f, r.top, r.right - 4.f, r.bottom),
                 hot ? th.fg : th.fg2, DWRITE_TEXT_ALIGNMENT_LEADING);
    }
}

Painter::Painter() : d_(new Impl) {}
Painter::~Painter() { delete d_; }

bool Painter::Init(ID2D1RenderTarget* rt, bool dark) {
    static const float kDots[2] = { 1.f, 3.f };
    if (!rt || !g_s.ok) return false;
    d_->rt = rt;
    d_->th = MakeTheme(dark);
    rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    if (FAILED(rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), &d_->br))) return false;
    g_s.d2d->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                               D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND), nullptr, 0, &d_->round);
    g_s.d2d->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT,
                               D2D1_CAP_STYLE_FLAT, D2D1_LINE_JOIN_MITER, 10.f, D2D1_DASH_STYLE_CUSTOM, 0.f),
                               kDots, 2, &d_->dotted);
    const std::vector<BYTE>& px = dark ? g_s.auroraDark : g_s.auroraLight;
    if (!px.empty()) {
        rt->CreateBitmap(D2D1::SizeU(g_s.aw, g_s.ah), px.data(), g_s.aw * 4,
                         D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                                                                  D2D1_ALPHA_MODE_PREMULTIPLIED), 96.f, 96.f),
                         &d_->aurora);
    }
    return true;
}

void Painter::Draw(const View& v, const Geometry& g) {
    Impl& d = *d_;
    if (!d.rt || !d.br) return;
    const float W = (float)g.width, H = (float)g.height;
    d.rt->SetTransform(D2D1::Matrix3x2F::Identity());
    d.rt->Clear(D2D1::ColorF(0, 0, 0, 0));

    // Everything is clipped to the rounded shape (the menu is overflow:hidden, radius 10).
    ComPtr<ID2D1RoundedRectangleGeometry> clip;
    ComPtr<ID2D1Layer> layer;
    const bool clipped = SUCCEEDED(g_s.d2d->CreateRoundedRectangleGeometry(
                             D2D1::RoundedRect(D2D1::RectF(0, 0, W, H), (float)kRadius, (float)kRadius), &clip)) &&
                         SUCCEEDED(d.rt->CreateLayer(nullptr, &layer));
    if (clipped)
        d.rt->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), clip.Get()), layer.Get());
    d.fill(D2D1::RectF(0, 0, W, H), d.th.menu);
    if (g.hasHead) d.drawHead(v, g);
    for (int y : g.hair) d.fill(D2D1::RectF((float)kBorder, (float)y, W - kBorder, (float)y + 1.f), d.th.rule);
    if (g.hasQs) d.drawQuick(v, g);
    d.drawBar(v, g);
    if (clipped) d.rt->PopLayer();
    d.ring(D2D1::RectF(0, 0, W, H), (float)kRadius, d.th.menub);
}

// ---------------------------------------------------------------- png

bool RenderToPng(const View& v, int profileTextW, int dpi, const wchar_t* path) {
    if (!DrawInit()) return false;
    const Geometry g = ComputeGeometry(v.perf, (int)v.sliders.size(), (int)v.toggles.size(), profileTextW);
    const UINT pw = (UINT)ScalePx(g.width, dpi), ph = (UINT)ScalePx(g.height, dpi);
    ComPtr<IWICBitmap> bmp;
    if (FAILED(g_s.wic->CreateBitmap(pw, ph, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bmp))) return false;
    ComPtr<ID2D1RenderTarget> rt;
    if (FAILED(g_s.d2d->CreateWicBitmapRenderTarget(
            bmp.Get(), D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                                                    D2D1::PixelFormat(DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_PREMULTIPLIED),
                                                    (float)dpi, (float)dpi), &rt))) return false;
    Painter p;
    if (!p.Init(rt.Get(), v.dark)) return false;
    rt->BeginDraw();
    p.Draw(v, g);
    if (FAILED(rt->EndDraw())) return false;

    ComPtr<IWICBitmapSource> conv;
    if (FAILED(WICConvertBitmapSource(GUID_WICPixelFormat32bppBGRA, bmp.Get(), &conv))) return false;
    ComPtr<IWICStream> stream;
    if (FAILED(g_s.wic->CreateStream(&stream)) || FAILED(stream->InitializeFromFilename(path, GENERIC_WRITE))) return false;
    ComPtr<IWICBitmapEncoder> enc;
    if (FAILED(g_s.wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc))) return false;
    if (FAILED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache))) return false;
    ComPtr<IWICBitmapFrameEncode> fr;
    if (FAILED(enc->CreateNewFrame(&fr, nullptr)) || FAILED(fr->Initialize(nullptr))) return false;
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    if (FAILED(fr->SetSize(pw, ph)) || FAILED(fr->SetPixelFormat(&fmt))) return false;
    if (FAILED(fr->WriteSource(conv.Get(), nullptr)) || FAILED(fr->Commit()) || FAILED(enc->Commit())) return false;
    return true;
}

}}  // namespace wind::Flyout
