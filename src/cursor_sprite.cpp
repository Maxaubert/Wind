#include "cursor_sprite.h"
#include "crosshair.h"
#include "band_window.h"
#include <cstring>
#include <cstdint>
#include <cmath>
#include <algorithm>
#include <vector>
#include <dwmapi.h>
namespace wind {

static const wchar_t* kClassName = L"WindCursorSprite";

// Bilinear upscale of a premultiplied BGRA buffer into the top-left of a larger canvas
// (issue #195). Premultiplied alpha makes the per-channel lerp correct (no color fringing at
// the edges). Used when the sprite self-scales (band-16 screen-space mode): DrawIconEx's own
// stretch is nearest-neighbor, which reads as "pixelated" the moment the scale is large -
// native Magnifier's pointer is smooth at any zoom, so render at native size and match it.
static void UpscaleBilinearPremul(const uint32_t* src, int sw, int sh,
                                  uint32_t* dst, int dstStride, int dw, int dh) {
    for (int y = 0; y < dh; y++) {
        const double fy = (y + 0.5) * sh / (double)dh - 0.5;
        int y0 = (int)std::floor(fy);
        const double wy = fy - y0;
        int y1 = y0 + 1;
        if (y0 < 0) y0 = 0;
        if (y0 > sh - 1) y0 = sh - 1;
        if (y1 > sh - 1) y1 = sh - 1;
        for (int x = 0; x < dw; x++) {
            const double fx = (x + 0.5) * sw / (double)dw - 0.5;
            int x0 = (int)std::floor(fx);
            const double wx = fx - x0;
            int x1 = x0 + 1;
            if (x0 < 0) x0 = 0;
            if (x0 > sw - 1) x0 = sw - 1;
            if (x1 > sw - 1) x1 = sw - 1;
            const uint32_t p00 = src[y0 * sw + x0], p01 = src[y0 * sw + x1];
            const uint32_t p10 = src[y1 * sw + x0], p11 = src[y1 * sw + x1];
            uint32_t out = 0;
            for (int sh8 = 0; sh8 < 32; sh8 += 8) {
                const double c = ((p00 >> sh8) & 0xFFu) * (1.0 - wx) * (1.0 - wy) +
                                 ((p01 >> sh8) & 0xFFu) * wx * (1.0 - wy) +
                                 ((p10 >> sh8) & 0xFFu) * (1.0 - wx) * wy +
                                 ((p11 >> sh8) & 0xFFu) * wx * wy;
                out |= ((uint32_t)(c + 0.5) & 0xFFu) << sh8;
            }
            dst[y * dstStride + x] = out;
        }
    }
}

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
    // immersive bands - the only way the cursor can cover the magnified taskbar / Start / tray,
    // and (band 17, issue #162) the Snipping Tool capture overlay. Without it the sprite is an
    // ordinary topmost window and the shell composites over it. Undocumented, so it is loaded
    // dynamically and cascades down to band 16 then plain topmost; see band_window.h.
    int usedBand = 0;
    hwnd_ = wind::CreateBandedWindow(exStyle, s_atom, L"WindCursor", WS_POPUP,
                                     0, 0, kSize, kSize, hInst, zorderBand, &usedBand);
    if (!hwnd_) {
        usedBand = 0;
        hwnd_ = CreateWindowExW(exStyle, kClassName, L"WindCursor", WS_POPUP,
                                0, 0, kSize, kSize, nullptr, nullptr, hInst, nullptr);
    }
    usedBand_ = hwnd_ ? usedBand : 0;
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

    // The on-screen object for standard cursors is blanked while magnifying; render from the
    // original shape we captured before blanking. A handle not in the map is an APP-CUSTOM
    // cursor (games!): it was never blanked, but the model hides the whole cursor plane via
    // MagShowSystemCursor, so we must render it ourselves too - the handle itself is a valid
    // shape source (issue #148: Foundation's cursor went unrendered and the raw cursor roamed).
    auto it = originals_.find(info.hCursor);
    HCURSOR shapeSource = (it != originals_.end()) ? it->second : info.hCursor;

    HICON hIconCopy = CopyIcon((HICON)shapeSource);
    if (hIconCopy == nullptr) return ShapeStatus::Hidden; // transient failure; don't imitate

    ICONINFO iconInfo{};
    if (!GetIconInfo(hIconCopy, &iconInfo)) {
        DestroyIcon(hIconCopy);
        return ShapeStatus::Hidden;
    }
    // These mask/color bitmaps are owned by us once GetIconInfo returns; read the native size
    // (needed to scale DrawIconEx) and the hotspot, then free them immediately - otherwise they
    // leak every tick the cursor shape changes. Mask-only cursors report a double-height mask.
    BITMAP bm{};
    if (iconInfo.hbmColor && GetObjectW(iconInfo.hbmColor, sizeof(bm), &bm)) {
        natW_ = bm.bmWidth; natH_ = bm.bmHeight;
    } else if (iconInfo.hbmMask && GetObjectW(iconInfo.hbmMask, sizeof(bm), &bm)) {
        natW_ = bm.bmWidth; natH_ = bm.bmHeight / 2;
    }
    if (natW_ <= 0 || natW_ > kSize) natW_ = 32;
    if (natH_ <= 0 || natH_ > kSize) natH_ = 32;
    if (iconInfo.hbmMask) DeleteObject(iconInfo.hbmMask);
    if (iconInfo.hbmColor) DeleteObject(iconInfo.hbmColor);
    int hotX = (int)iconInfo.xHotspot * scale_;   // hotspots live in FINAL (scaled) pixels
    int hotY = (int)iconInfo.yHotspot * scale_;

    // Render the shape ONCE at NATIVE size into a small DIB and cache the premultiplied pixels
    // (issue #195): every presented frame is then COMPOSED from this cache - bilinear-upscaled
    // in the self-scaling screen-space mode, or sub-pixel-shifted in the desktop-space mode
    // (the wobble fix) - so presentation never re-renders the icon.
    HDC screenDc = GetDC(nullptr);
    BITMAPINFO smi{};
    smi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    smi.bmiHeader.biWidth = natW_;
    smi.bmiHeader.biHeight = -natH_; // top-down DIB
    smi.bmiHeader.biPlanes = 1;
    smi.bmiHeader.biBitCount = 32;
    smi.bmiHeader.biCompression = BI_RGB;
    void* sbits = nullptr;
    HBITMAP sdib = CreateDIBSection(screenDc, &smi, DIB_RGB_COLORS, &sbits, nullptr, 0);
    if (sdib == nullptr || sbits == nullptr) {
        if (sdib != nullptr) DeleteObject(sdib);
        ReleaseDC(nullptr, screenDc);
        DestroyIcon(hIconCopy);
        return ShapeStatus::Hidden;
    }
    HDC sdc = CreateCompatibleDC(screenDc);
    HGDIOBJ sOld = SelectObject(sdc, sdib);
    // Zero before drawing so unpainted pixels are transparent. DrawIconEx with DI_NORMAL onto
    // a zeroed 32bpp DIB gives usable premultiplied alpha for cursors that carry their own
    // per-pixel alpha channel - this single pass is tried for every cursor, regardless of
    // whether GetIconInfo reported an hbmColor.
    memset(sbits, 0, (size_t)natW_ * natH_ * 4);
    DrawIconEx(sdc, 0, 0, hIconCopy, natW_, natH_, 0, nullptr, DI_NORMAL);
    // Mask/inversion cursors (the modern I-beam among them) come back fully transparent from
    // the single pass and fall to renderMaskShape below (detected by output, not by type).
    bool anyAlpha = false;
    const uint32_t* sp = (const uint32_t*)sbits;
    for (int i = 0; i < natW_ * natH_ && !anyAlpha; i++)
        if ((sp[i] & 0xFF000000u) != 0) anyAlpha = true;
    if (anyAlpha) {
        nativeShape_.assign(sp, sp + (size_t)natW_ * natH_);
        nsW_ = natW_; nsH_ = natH_;
    }
    SelectObject(sdc, sOld);
    DeleteDC(sdc);
    DeleteObject(sdib);
    ReleaseDC(nullptr, screenDc);

    DestroyIcon(iconCopy_); // destroy the previous copy we were holding
    iconCopy_ = hIconCopy;
    hotX_ = hotX;
    hotY_ = hotY;
    lastCursor_ = info.hCursor;
    lastVerdict_ = ShapeStatus::Rendered;
    crosshairMode_ = false;   // the window now holds the cursor shape again
    if (!anyAlpha)
        renderMaskShape();     // fills nativeShape_ from the mask classification, then composes
    else
        composeAndPresent();
    return ShapeStatus::Rendered;
}

// Sub-pixel shift of a premultiplied BGRA buffer into the top-left of a larger canvas
// (issue #195, the desktop-space wobble fix): dst(x,y) samples src at (x - fx, y - fy),
// bilinear, outside = transparent. A layered window can only sit on INTEGER desktop pixels,
// and under the fullscreen transform that residual (up to half a pixel) is magnified by the
// level - the +-10px-at-20x re-centering wobble the field reported. Baking the fraction into
// the CONTENT makes the sprite's effective position continuous, so the displayed cursor sits
// exactly on the lens point at any zoom.
static void ComposeShiftedPremul(const uint32_t* src, int sw, int sh,
                                 uint32_t* dst, int dstStride, int dw, int dh,
                                 double fx, double fy) {
    for (int y = 0; y < dh; y++) {
        const double sy = y - fy;
        const int y0 = (int)std::floor(sy);
        const double wy = sy - y0;
        for (int x = 0; x < dw; x++) {
            const double sx = x - fx;
            const int x0 = (int)std::floor(sx);
            const double wx = sx - x0;
            auto at = [&](int yy, int xx) -> uint32_t {
                return (yy < 0 || xx < 0 || yy >= sh || xx >= sw) ? 0u : src[yy * sw + xx];
            };
            const uint32_t p00 = at(y0, x0),     p01 = at(y0, x0 + 1);
            const uint32_t p10 = at(y0 + 1, x0), p11 = at(y0 + 1, x0 + 1);
            uint32_t out = 0;
            for (int sh8 = 0; sh8 < 32; sh8 += 8) {
                const double c = ((p00 >> sh8) & 0xFFu) * (1.0 - wx) * (1.0 - wy) +
                                 ((p01 >> sh8) & 0xFFu) * wx * (1.0 - wy) +
                                 ((p10 >> sh8) & 0xFFu) * (1.0 - wx) * wy +
                                 ((p11 >> sh8) & 0xFFu) * wx * wy;
                out |= ((uint32_t)(c + 0.5) & 0xFFu) << sh8;
            }
            dst[y * dstStride + x] = out;
        }
    }
}

// Present the cached native-res shape into the layered window: bilinear-upscaled when
// self-scaling (band-16 screen-space mode), sub-pixel-shifted at scale 1 (desktop-space mode).
// Cheap by construction - the icon render itself happens only in refreshShape/renderMaskShape;
// this is a small resample plus a 64px-class UpdateLayeredWindow (per tick during pans).
void CursorSprite::composeAndPresent() {
    if (hwnd_ == nullptr || nativeShape_.empty() || nsW_ <= 0 || nsH_ <= 0) return;
    const int S = bufSize();
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
    memset(bits, 0, (size_t)S * S * 4);
    uint32_t* canvas = (uint32_t*)bits;
    if (scale_ > 1) {
        const int dw = (std::min)(S, nsW_ * scale_), dh = (std::min)(S, nsH_ * scale_);
        UpscaleBilinearPremul(nativeShape_.data(), nsW_, nsH_, canvas, S, dw, dh);
    } else {
        const int dw = (std::min)(S, nsW_ + 1), dh = (std::min)(S, nsH_ + 1);
        ComposeShiftedPremul(nativeShape_.data(), nsW_, nsH_, canvas, S, dw, dh, fracX_, fracY_);
    }
    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.BlendFlags = 0;
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

// Sub-pixel positioning for the desktop-space mode (issue #195): integer base via SetWindowPos
// (deduped), fractional residual baked into the content (deduped on the fraction - an idle
// tick re-presents nothing). The hotspot lands exactly on (desktopX, desktopY) in continuous
// coordinates, so under the fullscreen transform the cursor sits pixel-exactly on the lens
// point at any zoom - no wobble, no stick-then-jump.
void CursorSprite::moveToSubpixel(double desktopX, double desktopY) {
    if (hwnd_ == nullptr) return;
    const int bx = (int)std::floor(desktopX);
    const int by = (int)std::floor(desktopY);
    const double fx = desktopX - bx, fy = desktopY - by;
    if (bx != lastBaseX_ || by != lastBaseY_) {
        lastBaseX_ = bx; lastBaseY_ = by;
        SetWindowPos(hwnd_, nullptr, bx - hotX_, by - hotY_, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (!crosshairMode_ && scale_ == 1 && (fx != fracX_ || fy != fracY_)) {
        fracX_ = fx; fracY_ = fy;
        composeAndPresent();
    }
}

// Change the sprite's integer zoom scale. Invalidates the shape cache so the next
// refreshShape()/showCrosshair() re-renders at the new size (hotspot recomputes too).
// Cap 20 = maxLevel's ceiling (the band-16 screen-space mode self-scales to the zoom level,
// issue #195); a 64*20 canvas is a 1280px layered surface, re-rendered only on integer steps.
void CursorSprite::setScale(int s) {
    if (s < 1) s = 1;
    if (s > 20) s = 20;
    if (s == scale_) return;
    scale_ = s;
    lastCursor_ = nullptr;
    crosshairMode_ = false;
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
    const int nw = natW_, nh = natH_;
    HDC screenDc = GetDC(nullptr);
    HDC memDc = CreateCompatibleDC(screenDc);
    // Classification runs at NATIVE size (issue #195): the two probe renders, the ink verdicts,
    // and the outline are all computed at the cursor's own resolution, then the finished premul
    // result is bilinear-upscaled into the canvas. Classifying an already-nearest-stretched
    // render would blockify the ink AND misplace the outline; this way the outline thickens with
    // the zoom smoothly, the same way the arrow's baked outline scales.
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = nw;
    bmi.bmiHeader.biHeight = -nh; // top-down DIB
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
    std::fill_n((uint32_t*)blackBits, (size_t)nw * nh, 0xFF000000u);
    DrawIconEx(memDc, 0, 0, iconCopy_, nw, nh, 0, nullptr, DI_NORMAL);

    SelectObject(memDc, whiteDib);
    std::fill_n((uint32_t*)whiteBits, (size_t)nw * nh, 0xFFFFFFFFu);
    DrawIconEx(memDc, 0, 0, iconCopy_, nw, nh, 0, nullptr, DI_NORMAL);

    const uint32_t kOpaqueWhite = 0xFFFFFFFFu;   // premultiplied opaque white (the ink)
    const uint32_t kOpaqueBlack = 0xFF000000u;   // premultiplied opaque black (the outline)
    uint32_t* black = (uint32_t*)blackBits;
    uint32_t* white = (uint32_t*)whiteBits;
    // shape[i] = 1 for every opaque pixel of the cursor: a baked colour pixel, or an inverting
    // pixel we are inking white. The outline pass below dilates this, so it must be recorded
    // BEFORE the outline is drawn, or the outline would feed on itself and keep growing.
    std::vector<uint8_t> shape((size_t)nw * nh);
    for (int i = 0; i < nw * nh; i++) {
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
    // One native pixel thick; the upscale below (or the fullscreen transform in desktop-space
    // mode) thickens it in step with the ink, the same way the arrow's own outline scales.
    for (int y = 0; y < nh; y++) {
        for (int x = 0; x < nw; x++) {
            int i = y * nw + x;
            if (shape[i]) continue;                  // already ink or baked colour
            bool touches = false;
            for (int dy = -1; dy <= 1 && !touches; dy++) {
                for (int dx = -1; dx <= 1 && !touches; dx++) {
                    int ny = y + dy, nx = x + dx;
                    if (ny < 0 || nx < 0 || ny >= nh || nx >= nw) continue;
                    if (shape[ny * nw + nx]) touches = true;
                }
            }
            if (touches) white[i] = kOpaqueBlack;
        }
    }

    // Cache the finished native-res result; composeAndPresent owns all presentation
    // (copy / upscale / sub-pixel shift) from here.
    nativeShape_.assign(white, white + (size_t)nw * nh);
    nsW_ = nw; nsH_ = nh;

    SelectObject(memDc, oldBmp);
    DeleteObject(whiteDib);
    DeleteObject(blackDib);
    DeleteDC(memDc);
    ReleaseDC(nullptr, screenDc);
    composeAndPresent();
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
    if (!hwnd_) return;
    if (!crosshairMode_) {
        renderCrosshair();
        crosshairMode_ = true;
        hotX_ = hotY_ = (bufSize() - 2) / 2;   // the cross centers on this texel (see BuildCrosshairBGRA)
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
