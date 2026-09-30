#include "cursor_tint.h"
#include "cursor_tint_pixels.h"
#include "logging.h"
#include <vector>

namespace wind {

// The standard system pointers (OCR_*), the same set the cursor blanker swaps.
static const UINT kIds[] = { 32512, 32513, 32514, 32515, 32516, 32642, 32643,
                             32644, 32645, 32646, 32648, 32649, 32650, 32651 };
// Animated in every stock scheme (busy, app starting): a static copy would freeze them.
static bool IsAnimatedId(UINT id) { return id == 32514 || id == 32650; }

static double MsSince(const LARGE_INTEGER& a) {
    LARGE_INTEGER b, f; QueryPerformanceCounter(&b); QueryPerformanceFrequency(&f);
    return double(b.QuadPart - a.QuadPart) * 1000.0 / double(f.QuadPart);
}

CursorTint::~CursorTint() { release(); }

void CursorTint::release() {
    for (auto& c : pristine_) if (c) { DestroyCursor(c); c = nullptr; }
}

void CursorTint::capture() {
    release();
    for (int i = 0; i < kCount; ++i) {
        HCURSOR shared = LoadCursorW(nullptr, MAKEINTRESOURCEW(kIds[i]));
        pristine_[i] = shared ? CopyCursor(shared) : nullptr;
        mono_[i] = false;
        ICONINFO ii{};
        if (pristine_[i] && GetIconInfo(pristine_[i], &ii)) {
            mono_[i] = ii.hbmColor == nullptr;
            if (ii.hbmMask) DeleteObject(ii.hbmMask);
            if (ii.hbmColor) DeleteObject(ii.hbmColor);
        }
    }
    applied_ = false;
    swapped_ = false;   // capture() runs right after a scheme reload: the pointers are the user's
}

// Reads a bitmap as top-down 32bpp (colour) or 1bpp (mask) rows.
static bool ReadBits(HDC dc, HBITMAP bm, int w, int h, int bpp, std::vector<uint8_t>& out, int& stride) {
    struct { BITMAPINFOHEADER h; RGBQUAD pal[2]; } bi{};
    bi.h.biSize = sizeof(BITMAPINFOHEADER);
    bi.h.biWidth = w; bi.h.biHeight = -h; bi.h.biPlanes = 1; bi.h.biBitCount = (WORD)bpp;
    bi.h.biCompression = BI_RGB;
    stride = ((w * bpp + 31) / 32) * 4;
    out.assign((size_t)stride * h, 0);
    return GetDIBits(dc, bm, 0, h, out.data(), reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS) == h;
}

// A tinted copy of `src`, same size and hotspot; nullptr on any failure (the caller keeps the original).
static HCURSOR BuildTinted(HCURSOR src, const ColorMatrix& m) {
    ICONINFO ii{};
    if (!GetIconInfo(src, &ii)) return nullptr;
    BITMAP bm{};
    GetObjectW(ii.hbmMask, sizeof(bm), &bm);
    const int w = bm.bmWidth, h = ii.hbmColor ? bm.bmHeight : bm.bmHeight / 2;
    HCURSOR out = nullptr;
    HDC dc = GetDC(nullptr);
    std::vector<uint32_t> px((size_t)w * h);
    bool ok = w > 0 && h > 0;
    std::vector<uint8_t> mask; int mStride = 0;
    if (ok) ok = ReadBits(dc, ii.hbmMask, w, ii.hbmColor ? h : h * 2, 1, mask, mStride);
    if (ok && ii.hbmColor) {
        std::vector<uint8_t> col; int cStride = 0;
        ok = ReadBits(dc, ii.hbmColor, w, h, 32, col, cStride);
        if (ok) {
            memcpy(px.data(), col.data(), px.size() * 4);
            if (!AnyAlpha(px.data(), (int)px.size())) AlphaFromMask(px.data(), w, h, mask.data(), mStride);
        }
    } else if (ok) {
        MonoToArgb(mask.data(), mask.data() + (size_t)mStride * h, mStride, w, h, px.data());
    }
    if (ok) {
        TintArgb(px.data(), (int)px.size(), m);
        BITMAPV5HEADER bh{};
        bh.bV5Size = sizeof(bh); bh.bV5Width = w; bh.bV5Height = -h; bh.bV5Planes = 1; bh.bV5BitCount = 32;
        bh.bV5Compression = BI_BITFIELDS;
        bh.bV5RedMask = 0x00FF0000; bh.bV5GreenMask = 0x0000FF00; bh.bV5BlueMask = 0x000000FF; bh.bV5AlphaMask = 0xFF000000;
        void* bits = nullptr;
        HBITMAP color = CreateDIBSection(dc, reinterpret_cast<BITMAPINFO*>(&bh), DIB_RGB_COLORS, &bits, nullptr, 0);
        HBITMAP andMask = CreateBitmap(w, h, 1, 1, nullptr);   // all 0: the alpha channel decides
        if (color && bits && andMask) {
            memcpy(bits, px.data(), px.size() * 4);
            // CreateBitmap leaves the bits undefined: clear the mask explicitly.
            std::vector<uint8_t> zero((size_t)(((w + 15) / 16) * 2) * h, 0);
            SetBitmapBits(andMask, (DWORD)zero.size(), zero.data());
            ICONINFO ni{};
            ni.fIcon = FALSE; ni.xHotspot = ii.xHotspot; ni.yHotspot = ii.yHotspot;
            ni.hbmMask = andMask; ni.hbmColor = color;
            out = (HCURSOR)CreateIconIndirect(&ni);
        }
        if (color) DeleteObject(color);
        if (andMask) DeleteObject(andMask);
    }
    ReleaseDC(nullptr, dc);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    return out;
}

void CursorTint::apply(const ColorMatrix& m) {
    if (IsIdentity(m)) { restore(true); return; }
    if (applied_ && SameMatrix(m, current_)) return;
    LARGE_INTEGER t0; QueryPerformanceCounter(&t0);
    int done = 0;
    for (int i = 0; i < kCount; ++i) {
        if (!pristine_[i] || IsAnimatedId(kIds[i])) continue;
        HCURSOR tinted = BuildTinted(pristine_[i], m);
        if (tinted && SetSystemCursor(tinted, kIds[i])) ++done;   // SetSystemCursor owns `tinted` on success
        else if (tinted) DestroyCursor(tinted);
    }
    applied_ = true;
    swapped_ = true;
    current_ = m;
    wind::Log(wind::LogLevel::Info, "color", "pointer tint applied (%d pointers, %.1f ms)", done, MsSince(t0));
}

void CursorTint::restore(bool reloadScheme) {
    if (reloadScheme ? !swapped_ : !applied_) return;
    LARGE_INTEGER t0; QueryPerformanceCounter(&t0);
    if (reloadScheme) {
        SystemParametersInfoW(SPI_SETCURSORS, 0, nullptr, 0);   // no broadcast: nothing else needs telling
        swapped_ = false;
    } else {
        for (int i = 0; i < kCount; ++i) {
            if (!pristine_[i] || IsAnimatedId(kIds[i])) continue;
            HCURSOR copy = CopyCursor(pristine_[i]);
            if (copy && !SetSystemCursor(copy, kIds[i])) DestroyCursor(copy);
        }
    }
    applied_ = false;
    wind::Log(wind::LogLevel::Info, "color", "pointer tint restored (%s, %.1f ms)",
              reloadScheme ? "scheme reload" : "direct swap", MsSince(t0));
}

}  // namespace wind
