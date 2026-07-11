#include "cursor_sprite.h"
#include "crosshair.h"
#include <cstring>
#include <cstdint>
#include <algorithm>
#include <dwmapi.h>
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
    inBand_ = (hwnd_ != nullptr);   // banded creation succeeded (composited INSIDE the magnification)
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
// pixels stay transparent, and inverting pixels are inked white and given a
// black outline (see renderMaskShape). The real system cursor is shown only
// while ShapeStatus::Hidden is returned, i.e. the cursor is suppressed/hidden
// or its shape could not be captured this tick.
CursorSprite::ShapeStatus CursorSprite::refreshShape() {
    CURSORINFO info{};
    info.cbSize = sizeof(CURSORINFO);
    if (!GetCursorInfo(&info)) return ShapeStatus::Hidden;
    if ((info.flags & CURSOR_SHOWING) == 0) return ShapeStatus::Hidden;
    if ((info.flags & CURSOR_SUPPRESSED) != 0) return ShapeStatus::Hidden;

    if (info.hCursor == lastCursor_) return lastVerdict_;

    // Standard cursors are blanked system-wide while magnifying (their shared handle's pixels are
    // transparent), so render those from the pre-blank copy captured in the originals map. A handle
    // NOT in the map is an app-loaded cursor (private handle - Explorer's hand, WinUI resize shapes,
    // app-custom pointers): its pixels are live and correct, so render from the handle itself.
    // MagShowSystemCursor(FALSE) hides the real cursor globally while the sprite is in use, so
    // drawing a private shape here never doubles up with a natively drawn one.
    auto it = originals_.find(info.hCursor);
    HCURSOR shapeSource = (it != originals_.end()) ? it->second : info.hCursor;

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
        renderMaskShape();
        crosshairMode_ = false;   // the window now holds the cursor shape again
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
    crosshairMode_ = false;   // the window now holds the cursor shape again
    return ShapeStatus::Rendered;
}

// Two-pass mask/inversion renderer for cursors whose single-pass render came
// back fully transparent (legacy AND/XOR mask cursors, e.g. the I-beam
// caret). Draws iconCopy_ once onto an opaque black-filled DIB and once onto
// an opaque white-filled DIB: pixels where both renders agree are opaque
// color pixels; pixels that stayed background-colored (white on black bg,
// black on white bg) are transparent; pixels that inverted (black on the
// white-bg render, white on the black-bg render) are the inverting/mask pixels.
//
// An inverting pixel has no colour of its own, so the sprite replacing it must
// choose one. Sampling the background to pick black or white ink cannot be made
// stable: a mixed or mid-grey background sits near the decision threshold, so
// the caret flicks between inks as it moves, and no dead-band or hysteresis
// removes that (it only shrinks the band where it happens, and leaves the caret
// low-contrast there). Instead paint the mask pixels white and synthesise a 1px
// black outline around the shape, which is what the standard arrow cursor does.
// Contrast then comes from the outline rather than from a guess about what lies
// underneath, so the caret is legible on any background and there is no verdict
// left to oscillate.
void CursorSprite::renderMaskShape() {
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

    const uint32_t kOpaqueWhite = 0xFFFFFFFFu;   // premultiplied opaque white (the ink)
    const uint32_t kOpaqueBlack = 0xFF000000u;   // premultiplied opaque black (the outline)
    uint32_t* black = (uint32_t*)blackBits;
    uint32_t* white = (uint32_t*)whiteBits;
    // shape[i] = 1 for every opaque pixel of the cursor: a baked colour pixel, or an inverting
    // pixel we are inking white. The outline pass below dilates this, so it must be recorded
    // BEFORE the outline is drawn, or the outline would feed on itself and keep growing.
    uint8_t shape[kSize * kSize];
    for (int i = 0; i < kSize * kSize; i++) {
        uint32_t rgbB = black[i] & 0x00FFFFFFu;
        uint32_t rgbW = white[i] & 0x00FFFFFFu;
        if (rgbB == rgbW) {
            white[i] = rgbB | 0xFF000000u; // opaque, alpha 255, premultiplied color from either render
            shape[i] = 1;
        } else {
            int lumB = (int)(rgbB & 0xFFu) + (int)((rgbB >> 8) & 0xFFu) + (int)((rgbB >> 16) & 0xFFu);
            int lumW = (int)(rgbW & 0xFFu) + (int)((rgbW >> 8) & 0xFFu) + (int)((rgbW >> 16) & 0xFFu);
            bool inverting = lumB > lumW;
            white[i] = inverting ? kOpaqueWhite : 0u;
            shape[i] = inverting ? 1 : 0;
        }
    }

    // Outline: every transparent pixel touching the shape (8-neighbourhood) becomes opaque black.
    // One desktop pixel thick, so the fullscreen transform magnifies it in step with the ink, the
    // same way the arrow cursor's own outline scales.
    for (int y = 0; y < kSize; y++) {
        for (int x = 0; x < kSize; x++) {
            int i = y * kSize + x;
            if (shape[i]) continue;                  // already ink or baked colour
            bool touches = false;
            for (int dy = -1; dy <= 1 && !touches; dy++) {
                for (int dx = -1; dx <= 1 && !touches; dx++) {
                    int ny = y + dy, nx = x + dx;
                    if (ny < 0 || nx < 0 || ny >= kSize || nx >= kSize) continue;
                    if (shape[ny * kSize + nx]) touches = true;
                }
            }
            if (touches) white[i] = kOpaqueBlack;
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

// True if a visible, non-cloaked window overlapping the sprite sits above it in z-order - i.e. a
// popup (tray/context menu, notification flyout, always-on-top app) has been raised over us. Walks
// the windows above us (GW_HWNDPREV); when we are already on top (the common case) the first
// GetWindow returns NULL and this is one cheap syscall. Same technique as RenderEngine's
// overlayDisplaced, scoped to the sprite's own small rect. Cloaked windows (another virtual desktop)
// and non-overlapping windows are ignored so we do not thrash SetWindowPos chasing them.
bool CursorSprite::displaced() const {
    if (!hwnd_) return false;
    HWND above = GetWindow(hwnd_, GW_HWNDPREV);
    if (!above) return false;
    RECT self{};
    if (!GetWindowRect(hwnd_, &self)) return false;
    for (; above; above = GetWindow(above, GW_HWNDPREV)) {
        if (!IsWindowVisible(above)) continue;
        int cloaked = 0;
        if (SUCCEEDED(DwmGetWindowAttribute(above, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked)
            continue;
        RECT wr, inter;
        if (GetWindowRect(above, &wr) && IntersectRect(&inter, &wr, &self)) return true;
    }
    return false;
}

// Reclaim top-of-band when displaced (immediate), plus a 1s unconditional backstop that self-heals
// if the displaced check ever misses a case. A banded window stays in its band across SetWindowPos,
// so this raises us to the top of our z-band without leaving it. Not done every tick: a per-tick
// z-order SetWindowPos synchronizes with the window manager and can microstutter (the same reason
// RenderEngine gates its re-assert). moveTo keeps SWP_NOZORDER so the common idle move stays cheap.
void CursorSprite::keepOnTop() {
    if (!hwnd_ || !visible_) return;
    unsigned long long nowMs = GetTickCount64();
    if (displaced() || nowMs - lastTopmostMs_ >= 1000) {
        lastTopmostMs_ = nowMs;
        SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
}

// Paints the Inspect crosshair (shared design: BuildCrosshairBGRA, same as the render model's
// sprite) into the layered window, premultiplied for UpdateLayeredWindow. The design centers at
// texel (kSize-2)/2's center, so the hotspot is that texel: moveTo() then puts the cross center
// (within half a pixel) on the look point.
void CursorSprite::renderCrosshair() {
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
        return;
    }
    HGDIOBJ oldBmp = SelectObject(memDc, dib);
    std::vector<uint32_t> px = BuildCrosshairBGRA(kSize, /*premultiply=*/true);
    memcpy(bits, px.data(), (size_t)kSize * kSize * 4);

    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;
    SIZE size{ kSize, kSize };
    POINT srcPt{ 0, 0 };
    UpdateLayeredWindow(hwnd_, nullptr, nullptr, &size, memDc, &srcPt, 0, &blend, ULW_ALPHA);

    SelectObject(memDc, oldBmp);
    DeleteObject(dib);
    DeleteDC(memDc);
    ReleaseDC(nullptr, screenDc);
}

void CursorSprite::showCrosshair() {
    if (!hwnd_) return;
    if (!crosshairMode_) {
        renderCrosshair();
        crosshairMode_ = true;
        hotX_ = hotY_ = (kSize - 2) / 2;   // the cross centers on this texel (see BuildCrosshairBGRA)
        // Invalidate the shape cache: the window no longer holds the cursor pixels, so the next
        // refreshShape() (Inspect off) must repaint even if the cursor HANDLE never changed -
        // otherwise its early-return would leave the crosshair on screen as the "cursor".
        lastCursor_ = nullptr;
    }
    show();
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
