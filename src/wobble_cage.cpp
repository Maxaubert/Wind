#include "wobble_cage.h"
#include "band_window.h"
#include <vector>
#include <cstdint>
#include <cmath>

namespace wind {
namespace {
const wchar_t* kClass = L"WindWobbleCage";
// Desktop-space half-extent, sized to hug a standard 32px cursor with a small margin. Fixed
// in desktop px so the window never has to be resized while zoomed (a resize is a DWM
// transaction; this tool must not perturb what it is measuring) and so the frame keeps its
// proportion to the cursor at every zoom - the cursor grows with the view, and so must this.
// The cursor's drawn body sits down-right of its hotspot; centring on the hotspot leaves the
// arrow in one quadrant (field screenshot 2026-08-22). Half a standard cursor re-centres it.
constexpr int kFlashMs = 250;
}

bool WobbleCage::create(int zorderBand) {
    HINSTANCE inst = GetModuleHandleW(nullptr);
    static ATOM s_atom = 0;
    if (!s_atom) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = inst;
        wc.lpszClassName = kClass;
        s_atom = RegisterClassExW(&wc);
    }
    const DWORD ex = WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE |
                     WS_EX_TOOLWINDOW;
    int used = 0;
    hwnd_ = wind::CreateBandedWindow(ex, s_atom, L"WindWobbleCage", WS_POPUP,
                                     0, 0, half_ * 2, half_ * 2, inst, zorderBand, &used);
    if (!hwnd_)
        hwnd_ = CreateWindowExW(ex, kClass, L"WindWobbleCage", WS_POPUP,
                                0, 0, half_ * 2, half_ * 2, nullptr, nullptr, inst, nullptr);
    return hwnd_ != nullptr;
}

void WobbleCage::destroy() {
    hide();
    if (hwnd_) { DestroyWindow(hwnd_); hwnd_ = nullptr; }
}

void WobbleCage::hide() {
    if (hwnd_ && visible_) { ShowWindow(hwnd_, SW_HIDE); visible_ = false; }
}

void WobbleCage::hits(unsigned& left, unsigned& right, unsigned& top, unsigned& bottom) const {
    left = hits_[0]; right = hits_[1]; top = hits_[2]; bottom = hits_[3];
}

// Paints the four bars into the layered window. Bars sit at the OUTSIDE of the frame so the
// cursor has clear room in the middle; a flashing side is drawn red and opaque, a resting side
// a dim grey so the frame stays readable without competing with the content.
void WobbleCage::redraw(double level) {
    if (!hwnd_) return;
    const int S = half_ * 2;
    // ~10 screen px, expressed in desktop px (a magnified window cannot draw sub-pixel).
    int t = (int)std::lround(10.0 / (level > 1.0 ? level : 1.0));
    if (t < 1) t = 1;
    if (t > half_ / 2) t = half_ / 2;

    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = S;
    bi.bmiHeader.biHeight = -S;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib) { DeleteDC(dc); ReleaseDC(nullptr, screen); return; }
    HGDIOBJ old = SelectObject(dc, dib);
    std::memset(bits, 0, (size_t)S * S * 4);

    const unsigned long long now = GetTickCount64();
    auto* px = static_cast<uint32_t*>(bits);
    // Premultiplied BGRA. Flashing: opaque red. Resting: dim white, enough to see the frame.
    auto colour = [&](int side) -> uint32_t {
        const bool hot = now < flashUntil_[side];
        return hot ? 0xFFFF0000u : 0x60606060u;
    };
    auto fill = [&](int x0, int y0, int x1, int y1, uint32_t c) {
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x)
                if (x >= 0 && x < S && y >= 0 && y < S) px[(size_t)y * S + x] = c;
    };
    // Vertical bars (left/right), horizontal bars (top/bottom) - the outside of the frame.
    fill(0, 0, t, S, colour(0));
    fill(S - t, 0, S, S, colour(1));
    fill(0, 0, S, t, colour(2));
    fill(0, S - t, S, S, colour(3));

    BLENDFUNCTION bf{};
    bf.BlendOp = AC_SRC_OVER;
    bf.SourceConstantAlpha = 255;
    bf.AlphaFormat = AC_SRC_ALPHA;
    SIZE sz{ S, S };
    POINT src{ 0, 0 };
    UpdateLayeredWindow(hwnd_, nullptr, nullptr, &sz, dc, &src, 0, &bf, ULW_ALPHA);

    SelectObject(dc, old);
    DeleteObject(dib);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
}

void WobbleCage::update(int desktopX, int desktopY, double level, double offX, double offY,
                        double gapScreenPx, bool clampedX, bool clampedY) {
    if (!hwnd_) return;
    // Collision: the sprite left the box. Numeric by necessity (see the header) - the cage
    // itself is magnified, so it cannot be the reference frame, only the indicator.
    const unsigned long long now = GetTickCount64();
    // OPPOSITE BARS ARE MUTUALLY EXCLUSIVE. The displacement cannot be left AND right at once,
    // so a hit on one side cancels the other's flash immediately - without that, a fast sign
    // flip inside the 250ms flash window lit both bars and the indicator read as nonsense
    // ("says it collides with the bottom and top bar which makes no sense", field 2026-08-22).
    int side = 0;
    if (!clampedX && offX < -gapScreenPx) { flashUntil_[0] = now + kFlashMs; flashUntil_[1] = 0; ++hits_[0]; side |= 1; }
    else if (!clampedX && offX > gapScreenPx) { flashUntil_[1] = now + kFlashMs; flashUntil_[0] = 0; ++hits_[1]; side |= 2; }
    if (!clampedY && offY < -gapScreenPx) { flashUntil_[2] = now + kFlashMs; flashUntil_[3] = 0; ++hits_[2]; side |= 4; }
    else if (!clampedY && offY > gapScreenPx) { flashUntil_[3] = now + kFlashMs; flashUntil_[2] = 0; ++hits_[3]; side |= 8; }

    // Repaint only when something changed: a flash started, one expired, or the zoom moved the
    // bar thickness. Painting every tick would add a layered-window update to the hot path.
    const bool anyHot = now < flashUntil_[0] || now < flashUntil_[1] ||
                        now < flashUntil_[2] || now < flashUntil_[3];
    const int state = (anyHot ? 1 : 0) | (side << 1);
    if (state != lastSide_ || std::abs(level - lastLevel_) > 0.05) {
        redraw(level);
        lastSide_ = state;
        lastLevel_ = level;
    }
    const int x = desktopX + body_ - half_, y = desktopY + body_ - half_;
    if (x != lastX_ || y != lastY_) {
        SetWindowPos(hwnd_, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        lastX_ = x; lastY_ = y;
    }
    if (!visible_) { ShowWindow(hwnd_, SW_SHOWNOACTIVATE); visible_ = true; }
    lastGap_ = gapScreenPx;
}

}
