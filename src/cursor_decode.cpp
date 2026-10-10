#include "cursor_decode.h"
#include "cursor_tint_pixels.h"
namespace wind {

// 1bpp mask rows, top-down, DWORD-aligned. `rows` is the number of rows to read.
static bool ReadMask1bpp(HDC hdc, HBITMAP bm, int w, int rows, std::vector<uint8_t>& out, int& stride) {
    struct { BITMAPINFOHEADER h; RGBQUAD pal[2]; } bi{};
    bi.h.biSize = sizeof(BITMAPINFOHEADER);
    bi.h.biWidth = w; bi.h.biHeight = -rows; bi.h.biPlanes = 1; bi.h.biBitCount = 1;
    bi.h.biCompression = BI_RGB;
    stride = ((w + 31) / 32) * 4;
    out.assign((size_t)stride * rows, 0);
    return GetDIBits(hdc, bm, 0, rows, out.data(), reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS) == rows;
}

// Decode an HCURSOR into top-down 32bpp BGRA (matches B8G8R8A8_UNORM memory order).
// Handles color cursors with per-pixel alpha (arrow, hand) and invert-style cursors with no
// alpha (e.g. the I-beam, which inverts the pixels beneath it). For invert cursors, isInvert
// is set and `out` is white where the glyph is / black elsewhere, to be drawn with an invert
// blend (drawing those opaque made the I-beam vanish on white input fields). Returns size +
// hotspot.
bool DecodeCursorBGRA(HCURSOR hc, std::vector<uint32_t>& out,
                     int& w, int& h, int& hotX, int& hotY, bool& isInvert) {
    ICONINFO ii{};
    if (!GetIconInfo(hc, &ii)) return false;
    hotX = (int)ii.xHotspot; hotY = (int)ii.yHotspot;
    isInvert = false;
    HDC hdc = GetDC(nullptr);
    BITMAP bm{};
    bool ok = false;
    if (ii.hbmColor) {
        GetObjectW(ii.hbmColor, sizeof(bm), &bm);
        w = bm.bmWidth; h = bm.bmHeight;
        out.assign((size_t)w * h, 0);
        BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;   // top-down
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
        GetDIBits(hdc, ii.hbmColor, 0, h, out.data(), &bi, DIB_RGB_COLORS);
        bool anyAlpha = false;
        for (uint32_t px : out) if (px & 0xFF000000u) { anyAlpha = true; break; }
        if (!anyAlpha) {
            // An old-style colour cursor: no alpha channel, its AND mask says which pixels are
            // opaque (the same rule CursorTint uses). Drawn as plain colour, not inverted.
            std::vector<uint8_t> andBits; int mStride = 0;
            if (ii.hbmMask && ReadMask1bpp(hdc, ii.hbmMask, w, h, andBits, mStride))
                AlphaFromMask(out.data(), w, h, andBits.data(), mStride);
            else
                for (uint32_t& px : out) px |= 0xFF000000u;   // no mask: opaque beats invisible
        }
        ok = true;
    } else if (ii.hbmMask) {
        GetObjectW(ii.hbmMask, sizeof(bm), &bm);
        w = bm.bmWidth; h = bm.bmHeight / 2;
        std::vector<uint32_t> both((size_t)w * bm.bmHeight, 0);
        BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -bm.bmHeight;
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
        GetDIBits(hdc, ii.hbmMask, 0, bm.bmHeight, both.data(), &bi, DIB_RGB_COLORS);
        out.assign((size_t)w * h, 0);
        // A pure-invert monochrome pointer (the classic I-beam) goes through the invert blend.
        std::vector<uint8_t> bits; int mStride = 0;
        if (ReadMask1bpp(hdc, ii.hbmMask, w, bm.bmHeight, bits, mStride) &&
            MonoIsPureInvert(bits.data(), bits.data() + (size_t)mStride * h, mStride, w, h)) {
            isInvert = true;
            for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x)
                out[(size_t)y * w + x] = MaskBit(bits.data() + (size_t)mStride * h, mStride, x, y)
                                         ? 0xFFFFFFFFu : 0x00000000u;
        }
        if (!isInvert)
        for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
            uint32_t andPx = both[(size_t)y * w + x] & 0xFFFFFFu;
            uint32_t xorPx = both[(size_t)(y + h) * w + x] & 0xFFFFFFu;
            uint32_t pix;
            if (andPx) pix = xorPx ? 0xFFFFFFFFu : 0x00000000u;  // transparent, or invert->white
            else       pix = xorPx ? 0xFFFFFFFFu : 0xFF000000u;  // white, or black
            out[(size_t)y * w + x] = pix;
        }
        ok = true;
    }
    ReleaseDC(nullptr, hdc);
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    return ok && w > 0 && h > 0;
}

}
