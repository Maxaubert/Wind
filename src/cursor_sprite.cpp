#include "cursor_sprite.h"
#include "tick_span.h"   // #361: per-tick spans
#include "crosshair.h"
#include "band_window.h"
#include <cstring>
#include <cstdint>
#include <algorithm>
#include <vector>
#include <dwmapi.h>
namespace wind {

static const wchar_t* kClassName = L"WindCursorSprite";

// A topmost click-through layered window that carries the Inspect crosshair. It is positioned in
// desktop coordinates on the look point, in the same tick that sets the fullscreen transform; the
// transform magnifies it together with the content beneath it, so crosshair and view are rigidly
// locked and cannot wobble against each other.

HWND CursorSprite::makeWindow(int band, int* usedBand) {
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
    // Match the render overlay's z-band (needs UIAccess) so the crosshair draws above the shell's
    // immersive bands - the only way it can cover the magnified taskbar / Start / tray. Without it
    // the sprite is an ordinary topmost window and the shell composites over it. Undocumented, so it is loaded
    // dynamically and cascades down to band 16 then plain topmost; see band_window.h.
    *usedBand = 0;
    HWND h = wind::CreateBandedWindow(exStyle, s_atom, L"WindCursor", WS_POPUP,
                                      0, 0, kSize, kSize, hInst, band, usedBand);
    if (!h) {
        *usedBand = 0;
        h = CreateWindowExW(exStyle, kClassName, L"WindCursor", WS_POPUP,
                            0, 0, kSize, kSize, nullptr, nullptr, hInst, nullptr);
    }
    if (h) {
        // Exempt the sprite from Aero Peek, exactly like the render overlay (issue #141). Resting
        // on a taskbar thumbnail makes DWM preview that window and hide every other one, and no
        // z-band beats that: it is a compositor effect. Unexempted, the cursor showed over the
        // thumbnail and then vanished ~0.5 s later, on every hover (issue #267, owner recording).
        BOOL exPeek = TRUE;
        DwmSetWindowAttribute(h, DWMWA_EXCLUDED_FROM_PEEK, &exPeek, sizeof(exPeek));
        // Invisible to screen capture, like the real Windows pointer, which screenshots never
        // contain. Captured, the sprite was frozen into the Snipping Tool's screenshot and panned
        // with the picture (issue #269, reproduced).
        SetWindowDisplayAffinity(h, WDA_EXCLUDEFROMCAPTURE);
    }
    return h;
}

bool CursorSprite::create(int zorderBand, bool autoHigh) {
    int used = 0;
    hwndLow_ = makeWindow(zorderBand, &used);
    usedBand_ = hwndLow_ ? used : 0;
    // The band-16 twin (issue #269). Only worth having when the low window is below 16 and the
    // band is actually granted (UIAccess): a refused request falls through to plain topmost,
    // which would be a second LOW window, so that one is discarded.
    if (hwndLow_ && autoHigh && usedBand_ < kSpriteHighBand) {
        int usedHigh = 0;
        hwndHigh_ = makeWindow(kSpriteHighBand, &usedHigh);
        if (hwndHigh_ && usedHigh != kSpriteHighBand) {
            DestroyWindow(hwndHigh_);
            hwndHigh_ = nullptr;
        }
    }
    hwnd_ = hwndLow_;
    layer_ = SpriteLayer::Low;
    return hwnd_ != nullptr;
}

void CursorSprite::setLayer(SpriteLayer l) {
    if (l == layer_) return;
    HWND to = (l == SpriteLayer::High) ? hwndHigh_ : hwndLow_;
    if (!to) return;
    // A switch back before the previous one finished: the window we return to is the pending
    // one, so it must not be hidden by the next show().
    if (pendingHide_ == to) pendingHide_ = nullptr;
    HWND from = hwnd_;
    hwnd_ = to;
    layer_ = l;
    // The incoming window holds stale or no pixels: force the next showCrosshair() to paint it.
    crosshairMode_ = false;
    reapplyPosition();
    if (visible_) {
        visible_ = false;          // so the next show() reveals the incoming window...
        pendingHide_ = from;       // ...and only then hides this one
    }
}

// Moves the sprite so its hotspot sits at the given desktop point. The target is remembered:
// painting the crosshair sets a NEW hotspot, and the window must then be repositioned even though
// the target point never moved (issue #229).
void CursorSprite::moveTo(int desktopX, int desktopY) {
    wind::SpanScope span(wind::kSpanSprite);
    lastTargetX_ = desktopX; lastTargetY_ = desktopY; haveTarget_ = true;
    SetWindowPos(hwnd_, nullptr, desktopX - hotX_, desktopY - hotY_, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

// Re-applies the remembered target with the current hotspot (called after a repaint that changed
// hotX_/hotY_).
void CursorSprite::reapplyPosition() {
    if (haveTarget_) {
        SetWindowPos(hwnd_, nullptr, lastTargetX_ - hotX_, lastTargetY_ - hotY_, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
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
    // Displaced-check only - NO periodic backstop: an unconditional SetWindowPos(TOPMOST) is a
    // synchronous DWM z-order transaction that hitches a fullscreen game (issue #148; same fix
    // as the render overlay's calm-topmost). The per-tick displaced() walk reclaims immediately.
    if (displaced()) {
        lastTopmostMs_ = GetTickCount64();
        SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
}

// Paints the Inspect crosshair (shared design: BuildCrosshairBGRA, same as the render model's
// sprite) into the layered window, premultiplied for UpdateLayeredWindow. The design centers at
// texel (kSize-2)/2's center, so the hotspot is that texel: moveTo() then puts the cross center
// (within half a pixel) on the look point.
void CursorSprite::renderCrosshair() {
    const int S = bufSize();   // crosshair fills the whole (scaled) canvas: it grows with zoom too
    HDC screenDc = GetDC(nullptr);
    HDC memDc = CreateCompatibleDC(screenDc);
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = S;
    bmi.bmiHeader.biHeight = -S; // top-down DIB
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
    std::vector<uint32_t> px = BuildCrosshairBGRA(S, /*premultiply=*/true);
    memcpy(bits, px.data(), (size_t)S * S * 4);

    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;
    SIZE size{ S, S };
    POINT srcPt{ 0, 0 };
    UpdateLayeredWindow(hwnd_, nullptr, nullptr, &size, memDc, &srcPt, 0, &blend, ULW_ALPHA);

    SelectObject(memDc, oldBmp);
    DeleteObject(dib);
    DeleteDC(memDc);
    ReleaseDC(nullptr, screenDc);
}

void CursorSprite::showCrosshair() {
    wind::SpanScope span(wind::kSpanSprite);
    if (!hwnd_) return;
    if (!crosshairMode_) {
        renderCrosshair();
        crosshairMode_ = true;
        hotX_ = hotY_ = (bufSize() - 2) / 2;   // the cross centers on this texel (see BuildCrosshairBGRA)
        reapplyPosition();                     // new hotspot, same target (issue #229)
    }
    show();
}

void CursorSprite::show() {
    wind::SpanScope span(wind::kSpanSprite);
    if (!visible_) { ShowWindow(hwnd_, SW_SHOWNOACTIVATE); visible_ = true; }
    if (pendingHide_) { ShowWindow(pendingHide_, SW_HIDE); pendingHide_ = nullptr; }
}
void CursorSprite::hide() {
    wind::SpanScope span(wind::kSpanSprite);
    if (visible_) { ShowWindow(hwnd_, SW_HIDE); visible_ = false; }
    if (pendingHide_) { ShowWindow(pendingHide_, SW_HIDE); pendingHide_ = nullptr; }
}

void CursorSprite::destroy() {
    hide();
    if (hwndLow_)  { DestroyWindow(hwndLow_);  hwndLow_ = nullptr; }
    if (hwndHigh_) { DestroyWindow(hwndHigh_); hwndHigh_ = nullptr; }
    hwnd_ = nullptr;
}
}
