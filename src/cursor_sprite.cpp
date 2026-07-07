#include "cursor_sprite.h"
#include <cstring>
#include <cstdint>
#include <algorithm>
namespace wind {

static const wchar_t* kClassName = L"WindCursorSprite";

// A topmost click-through layered window that mirrors the system cursor.
// While magnifying, the real cursor is hidden and this sprite is positioned
// in unmagnified desktop coordinates at the tracked cursor position, in the
// same tick that sets the fullscreen transform. The transform magnifies the
// sprite together with the content beneath it, so cursor and view are
// rigidly locked and cannot wobble against each other.

bool CursorSprite::create(int zorderBand) {
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    // Register once and keep the atom: RegisterClassExW returns 0 on a re-register (class is
    // process-global and never unregistered), and CreateWindowInBand needs a valid atom, so cache it.
    static ATOM s_atom = 0;
    if (!s_atom) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = hInst;
        wc.lpszClassName = kClassName;
        s_atom = RegisterClassExW(&wc);
    }

    const DWORD exStyle = WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT
                        | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;
    hwnd_ = nullptr;
    // Match the render overlay's z-band (needs UIAccess) so the sprite draws above the shell's
    // immersive bands - the only way the cursor can cover the magnified taskbar / Start / tray.
    // Without it the sprite is an ordinary topmost window and the shell composites over it.
    // Undocumented, so load it dynamically and fall back to a plain topmost window when the band
    // API or UIAccess is unavailable (e.g. the non-elevated dev build).
    if (zorderBand > 0 && s_atom) {
        using PFN_CWIB = HWND(WINAPI*)(DWORD, ATOM, LPCWSTR, DWORD, int, int, int, int,
                                       HWND, HMENU, HINSTANCE, LPVOID, DWORD);
        if (HMODULE u32 = GetModuleHandleW(L"user32.dll")) {
            if (auto pCWIB = reinterpret_cast<PFN_CWIB>(GetProcAddress(u32, "CreateWindowInBand"))) {
                hwnd_ = pCWIB(exStyle, s_atom, L"WindCursor", WS_POPUP,
                              0, 0, kSize, kSize, nullptr, nullptr, hInst, nullptr,
                              static_cast<DWORD>(zorderBand));
            }
        }
    }
    if (!hwnd_) {
        hwnd_ = CreateWindowExW(exStyle, kClassName, L"WindCursor", WS_POPUP,
                                0, 0, kSize, kSize, nullptr, nullptr, hInst, nullptr);
    }
    return hwnd_ != nullptr;
}

// Re-evaluates the system cursor and, for shapes that can be rendered
// faithfully, repaints the layered sprite bitmap. Cursors whose single-pass
// render comes back fully transparent (no per-pixel alpha - the modern
// I-beam caret among them) are rendered with a two-pass mask/inversion
// technique instead: opaque pixels keep their color, genuinely transparent
// pixels stay transparent, and inverting pixels are painted with a solid
// polarity color chosen from the desktop background (see needsPolarity()
// and setPolarity()). The real system cursor is shown only while
// ShapeStatus::Hidden is returned, i.e. the cursor is suppressed/hidden or
// its shape could not be captured this tick.
CursorSprite::ShapeStatus CursorSprite::refreshShape() {
    CURSORINFO info{};
    info.cbSize = sizeof(CURSORINFO);
    if (!GetCursorInfo(&info)) return ShapeStatus::Hidden;
    if ((info.flags & CURSOR_SHOWING) == 0) return ShapeStatus::Hidden;
    if ((info.flags & CURSOR_SUPPRESSED) != 0) return ShapeStatus::Hidden;

    if (info.hCursor == lastCursor_) return lastVerdict_;

    // The on-screen object for standard cursors is blanked while
    // magnifying; render from the original shape we captured before
    // blanking instead. A handle not in the map is an app-custom cursor
    // that was never blanked - it is drawn natively and visibly.
    auto it = originals_.find(info.hCursor);
    HCURSOR shapeSource = (it != originals_.end()) ? it->second : nullptr;
    if (shapeSource == nullptr) {
        lastCursor_ = info.hCursor;
        lastVerdict_ = ShapeStatus::Unsupported;
        return ShapeStatus::Unsupported;
    }

    HICON hIconCopy = CopyIcon((HICON)shapeSource);
    if (hIconCopy == nullptr) return ShapeStatus::Hidden; // transient failure; don't imitate

    ICONINFO iconInfo{};
    if (!GetIconInfo(hIconCopy, &iconInfo)) {
        DestroyIcon(hIconCopy);
        return ShapeStatus::Hidden;
    }
    // These mask/color bitmaps are owned by us once GetIconInfo returns; we only
    // need the hotspot, so free them immediately - otherwise they leak every tick
    // the cursor shape changes.
    if (iconInfo.hbmMask) DeleteObject(iconInfo.hbmMask);
    if (iconInfo.hbmColor) DeleteObject(iconInfo.hbmColor);
    int hotX = (int)iconInfo.xHotspot;
    int hotY = (int)iconInfo.yHotspot;

    HDC screenDc = GetDC(nullptr);
    HDC memDc = CreateCompatibleDC(screenDc);
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = kSize;
    bmi.bmiHeader.biHeight = -kSize; // top-down DIB
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screenDc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (dib == nullptr || bits == nullptr) {
        if (dib != nullptr) DeleteObject(dib);
        DeleteDC(memDc);
        ReleaseDC(nullptr, screenDc);
        DestroyIcon(hIconCopy);
        return ShapeStatus::Hidden;
    }
    HGDIOBJ oldBmp = SelectObject(memDc, dib);

    // Zero the bits buffer before drawing so unpainted pixels are
    // transparent rather than whatever garbage the allocation happened to
    // contain. DrawIconEx with DI_NORMAL onto a zeroed 32bpp DIB gives
    // usable premultiplied alpha for cursors that carry their own
    // per-pixel alpha channel - this single pass is tried for every
    // cursor, regardless of whether GetIconInfo reported an hbmColor.
    memset(bits, 0, (size_t)kSize * kSize * 4);
    DrawIconEx(memDc, 0, 0, hIconCopy, 0, 0, 0, nullptr, DI_NORMAL);

    // Some cursors (the modern I-beam among them) report a color bitmap
    // via GetIconInfo, but that color bitmap's alpha channel is empty, so
    // the single pass above yields a fully transparent result (every
    // pixel's alpha byte is 0). Detect the all-transparent case here, by
    // output rather than by type, and fall back to the two-pass
    // mask/inversion renderer for these mask cursors.
    bool anyAlpha = false;
    uint32_t* pixels = (uint32_t*)bits;
    for (int i = 0; i < kSize * kSize; i++) {
        if ((pixels[i] & 0xFF000000u) != 0) { anyAlpha = true; break; }
    }

    if (!anyAlpha) {
        SelectObject(memDc, oldBmp);
        DeleteObject(dib);
        DeleteDC(memDc);
        ReleaseDC(nullptr, screenDc);

        DestroyIcon(iconCopy_); // destroy the previous copy we were holding
        iconCopy_ = hIconCopy;
        hotX_ = hotX;
        hotY_ = hotY;
        lastCursor_ = info.hCursor;
        lastVerdict_ = ShapeStatus::Rendered;
        needsPolarity_ = true;
        renderMaskShape(lastPolarityDark_);
        return ShapeStatus::Rendered;
    }

    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.BlendFlags = 0;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;
    SIZE size{ kSize, kSize };
    POINT srcPt{ 0, 0 };
    UpdateLayeredWindow(hwnd_, nullptr, nullptr, &size, memDc, &srcPt, 0, &blend, ULW_ALPHA);

    SelectObject(memDc, oldBmp);
    DeleteObject(dib);
    DeleteDC(memDc);
    ReleaseDC(nullptr, screenDc);

    DestroyIcon(iconCopy_); // destroy the previous copy we were holding
    iconCopy_ = hIconCopy;
    hotX_ = hotX;
    hotY_ = hotY;
    lastCursor_ = info.hCursor;
    lastVerdict_ = ShapeStatus::Rendered;
    needsPolarity_ = false;
    return ShapeStatus::Rendered;
}

// Re-renders the current mask shape if the polarity changed. darkCursor
// true = draw inverting pixels black (light background).
void CursorSprite::setPolarity(bool darkCursor) {
    if (!needsPolarity_ || darkCursor == lastPolarityDark_) return;
    lastPolarityDark_ = darkCursor;
    renderMaskShape(darkCursor);
}

// Two-pass mask/inversion renderer for cursors whose single-pass render came
// back fully transparent (legacy AND/XOR mask cursors, e.g. the I-beam
// caret). Draws iconCopy_ once onto an opaque black-filled DIB and once onto
// an opaque white-filled DIB: pixels where both renders agree are opaque
// color pixels; pixels that stayed background-colored (white on black bg,
// black on white bg) are transparent; pixels that inverted (black on the
// white-bg render, white on the black-bg render) are the inverting/mask
// pixels, painted solid with the given polarity color and no outline or halo.
void CursorSprite::renderMaskShape(bool darkCursor) {
    HDC screenDc = GetDC(nullptr);
    HDC memDc = CreateCompatibleDC(screenDc);
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = kSize;
    bmi.bmiHeader.biHeight = -kSize; // top-down DIB
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* blackBits = nullptr;
    void* whiteBits = nullptr;
    HBITMAP blackDib = CreateDIBSection(screenDc, &bmi, DIB_RGB_COLORS, &blackBits, nullptr, 0);
    HBITMAP whiteDib = nullptr;
    if (blackDib != nullptr) whiteDib = CreateDIBSection(screenDc, &bmi, DIB_RGB_COLORS, &whiteBits, nullptr, 0);
    if (blackDib == nullptr || whiteDib == nullptr) {
        if (blackDib != nullptr) DeleteObject(blackDib);
        DeleteDC(memDc);
        ReleaseDC(nullptr, screenDc);
        return;
    }

    HGDIOBJ oldBmp = SelectObject(memDc, blackDib);
    std::fill_n((uint32_t*)blackBits, (size_t)kSize * kSize, 0xFF000000u);
    DrawIconEx(memDc, 0, 0, iconCopy_, 0, 0, 0, nullptr, DI_NORMAL);

    SelectObject(memDc, whiteDib);
    std::fill_n((uint32_t*)whiteBits, (size_t)kSize * kSize, 0xFFFFFFFFu);
    DrawIconEx(memDc, 0, 0, iconCopy_, 0, 0, 0, nullptr, DI_NORMAL);

    uint32_t ink = darkCursor ? 0xFF000000u : 0xFFFFFFFFu; // opaque black or premultiplied opaque white
    uint32_t* black = (uint32_t*)blackBits;
    uint32_t* white = (uint32_t*)whiteBits;
    for (int i = 0; i < kSize * kSize; i++) {
        uint32_t rgbB = black[i] & 0x00FFFFFFu;
        uint32_t rgbW = white[i] & 0x00FFFFFFu;
        if (rgbB == rgbW) {
            white[i] = rgbB | 0xFF000000u; // opaque, alpha 255, premultiplied color from either render
        } else {
            int lumB = (int)(rgbB & 0xFFu) + (int)((rgbB >> 8) & 0xFFu) + (int)((rgbB >> 16) & 0xFFu);
            int lumW = (int)(rgbW & 0xFFu) + (int)((rgbW >> 8) & 0xFFu) + (int)((rgbW >> 16) & 0xFFu);
            white[i] = lumB > lumW ? ink : 0u; // inverting -> polarity ink; else transparent
        }
    }

    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.BlendFlags = 0;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;
    SIZE size{ kSize, kSize };
    POINT srcPt{ 0, 0 };
    UpdateLayeredWindow(hwnd_, nullptr, nullptr, &size, memDc, &srcPt, 0, &blend, ULW_ALPHA);

    SelectObject(memDc, oldBmp);
    DeleteObject(whiteDib);
    DeleteObject(blackDib);
    DeleteDC(memDc);
    ReleaseDC(nullptr, screenDc);
}

// Moves the sprite so its hotspot sits at the given desktop point.
void CursorSprite::moveTo(int desktopX, int desktopY) {
    SetWindowPos(hwnd_, nullptr, desktopX - hotX_, desktopY - hotY_, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void CursorSprite::show() { if (!visible_) { ShowWindow(hwnd_, SW_SHOWNOACTIVATE); visible_ = true; } }
void CursorSprite::hide() { if (visible_) { ShowWindow(hwnd_, SW_HIDE); visible_ = false; } }

void CursorSprite::destroy() {
    hide();
    if (hwnd_) { DestroyWindow(hwnd_); hwnd_ = nullptr; }
    DestroyIcon(iconCopy_);
    iconCopy_ = nullptr;
}
}
