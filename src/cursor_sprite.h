#pragma once
#include <windows.h>
#include <unordered_map>
#include <cstdint>
#include <vector>
namespace wind {
class CursorSprite {
public:
    enum class ShapeStatus { Rendered, Hidden, Unsupported };
    explicit CursorSprite(const std::unordered_map<HCURSOR, HCURSOR>& originals) : originals_(originals) {}
    bool create(int zorderBand = 0);   // >0 -> CreateWindowInBand (above the shell; needs UIAccess)
    // The band the window ACTUALLY got (the cascade can refuse the request; band_window logs
    // it). Callers keying behavior to a band (spriteBand16 screen-space positioning) must read
    // this, never the requested value - a refused band with requested-keyed behavior mispositions.
    int usedBand() const { return usedBand_; }
    ShapeStatus refreshShape();
    void moveTo(int desktopX, int desktopY);
    // Desktop-space positioning with the fractional residual baked into the sprite CONTENT
    // (issue #195): a layered window sits on integer desktop pixels, and under the fullscreen
    // transform that half-pixel residual is magnified by the level - the re-centering wobble /
    // stick-then-jump inertia the field reported. The hotspot lands on the continuous point.
    void moveToSubpixel(double desktopX, double desktopY);
    void show();
    void hide();
    // Re-assert HWND_TOPMOST when a window has been displaced above us, throttled with a 1s backstop.
    // The sprite is composited OUTSIDE the fullscreen magnification, so it competes in real z-order
    // with real windows (tray/context menus, notification flyouts, other always-on-top apps); without
    // this it is raised once at create() and any topmost popup that appears later stays over it. Call
    // each active tick while shown. Mirrors RenderEngine's overlayDisplaced re-assert.
    void keepOnTop();
    // Inspect mode: repaint the sprite as the crosshair (BuildCrosshairBGRA, the same design the
    // render model draws) and show it. The hotspot becomes the cross center, so moveTo() places the
    // crosshair ON the look point. Cached: repaints only on the first call after normal-cursor use;
    // the next refreshShape() repaints the cursor shape, so leaving Inspect needs no explicit reset.
    void showCrosshair();
    // Integer zoom scale for the sprite (1..20). Only meaningful when the sprite composites
    // OUTSIDE the fullscreen magnification (band-16 screen-space mode) - there, matching the
    // zoom is our job: the cursor/crosshair is re-rendered scale x larger on integer change,
    // bilinear-upscaled from a native-size render so it stays smooth, never blocky (#195).
    // In desktop-space mode DWM magnifies the sprite itself; leave scale at 1 there.
    void setScale(int s);
    void destroy();
private:
    int usedBand_ = 0;
    void renderMaskShape();
    void renderCrosshair();
    void composeAndPresent();          // present the cached native shape (upscale / sub-pixel shift)
    bool displaced() const;            // a visible, overlapping window sits above us in z-order
    static const int kSize = 64;       // base (1x) logical canvas; buffers are kSize * scale_
    int bufSize() const { return kSize * scale_; }
    const std::unordered_map<HCURSOR, HCURSOR>& originals_;
    HWND    hwnd_ = nullptr;
    HCURSOR lastCursor_ = nullptr;
    ShapeStatus lastVerdict_ = ShapeStatus::Hidden;
    HICON   iconCopy_ = nullptr;
    int     hotX_ = 0, hotY_ = 0;      // in FINAL (scaled) sprite pixels
    int     natW_ = 0, natH_ = 0;      // icon's native size (DrawIconEx scales to nat * scale_)
    int     scale_ = 1;                // current integer zoom scale (1..20)
    std::vector<uint32_t> nativeShape_;   // cached native-res premultiplied shape (compose source)
    int     nsW_ = 0, nsH_ = 0;           // cached shape dimensions
    double  fracX_ = 0.0, fracY_ = 0.0;   // sub-pixel residual baked into the content
    int     lastBaseX_ = INT_MIN, lastBaseY_ = INT_MIN;   // dedupe the integer SetWindowPos
    bool    visible_ = false;
    bool    crosshairMode_ = false;          // window currently holds the crosshair pixels
    unsigned long long lastTopmostMs_ = 0;   // last HWND_TOPMOST re-assert (throttled)
};
}
