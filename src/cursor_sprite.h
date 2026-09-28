#pragma once
#include <windows.h>
#include <unordered_map>
#include "sprite_layer.h"
namespace wind {
class CursorSprite {
public:
    enum class ShapeStatus { Rendered, Hidden, Unsupported };
    explicit CursorSprite(const std::unordered_map<HCURSOR, HCURSOR>& originals) : originals_(originals) {}
    // zorderBand > 0 -> CreateWindowInBand (above the shell; needs UIAccess). autoHigh (issue
    // #269) also creates a second window in band 16; setLayer() then picks which one shows.
    // capturable: leave the windows visible to screen capture (the dualcursor test rig only).
    bool create(int zorderBand = 0, bool autoHigh = false, bool capturable = false);
    // Automatic band (issue #269): the low window (zorderBand, as before) and a band-16 one hold
    // the same shape at the same place, and exactly one is ever shown. Switching invalidates the
    // shape cache, so the next refreshShape()/showCrosshair() paints the incoming window, and the
    // next show() reveals it BEFORE hiding the outgoing one: no frame without a cursor.
    bool hasHigh() const { return hwndHigh_ != nullptr; }
    int  highBand() const { return usedBandHigh_; }
    SpriteLayer layer() const { return layer_; }
    void setLayer(SpriteLayer l);
    // The band the window ACTUALLY got (the cascade can refuse the request; band_window logs
    // it). Callers keying behavior to a band (spriteBand16 screen-space positioning) must read
    // this, never the requested value - a refused band with requested-keyed behavior mispositions.
    int usedBand() const { return usedBand_; }
    ShapeStatus refreshShape();
    void moveTo(int desktopX, int desktopY);
    void reapplyPosition();   // re-place after a hotspot-changing re-render (issue #229)
   // For the coherent-sprite path (issue #229): the hook repositions this window itself, from
    // the same event position it writes the transform with.
    HWND hwnd() const { return hwnd_; }
    int  hotX() const { return hotX_; }
    int  hotY() const { return hotY_; }
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
    // Integer zoom scale for the sprite (1..8). The sprite composites OUTSIDE the fullscreen
    // magnification (unmagnified), so matching the zoom is our job: the cursor/crosshair is
    // re-rendered scale x larger on change (issue #148: "cursor should grow as you zoom").
    void setScale(int s);
    void destroy();
private:
    int usedBand_ = 0;
    int usedBandHigh_ = 0;
    HWND makeWindow(int band, int* usedBand, bool capturable);
    HWND hwndLow_ = nullptr;           // zorderBand (band 2 under UIAccess by default)
    HWND hwndHigh_ = nullptr;          // band 16, only with autoHigh
    HWND pendingHide_ = nullptr;       // the outgoing window after a switch, hidden by show()/hide()
    SpriteLayer layer_ = SpriteLayer::Low;
    void renderMaskShape();
    void renderCrosshair();
    bool displaced() const;            // a visible, overlapping window sits above us in z-order
    static const int kSize = 64;       // base (1x) logical canvas; buffers are kSize * scale_
    int bufSize() const { return kSize * scale_; }
    const std::unordered_map<HCURSOR, HCURSOR>& originals_;
    HWND    hwnd_ = nullptr;           // the active window: hwndLow_ or hwndHigh_
    HCURSOR lastCursor_ = nullptr;
    ShapeStatus lastVerdict_ = ShapeStatus::Hidden;
    HICON   iconCopy_ = nullptr;
    int     hotX_ = 0, hotY_ = 0;      // in FINAL (scaled) sprite pixels
    int     natW_ = 0, natH_ = 0;      // icon's native size (DrawIconEx scales to nat * scale_)
    int     lastTargetX_ = 0, lastTargetY_ = 0;   // last moveTo target (hotspot-independent)
    bool    haveTarget_ = false;
    int     scale_ = 1;                // current integer zoom scale (1..8)
    bool    visible_ = false;
    bool    crosshairMode_ = false;          // window currently holds the crosshair pixels
    unsigned long long lastTopmostMs_ = 0;   // last HWND_TOPMOST re-assert (throttled)
};
}
