#pragma once
#include <windows.h>
#include "sprite_layer.h"
namespace wind {
// The Inspect crosshair window. Since the native cursor (issue #369) DWM draws the real pointer, so
// this window never renders a cursor shape: it is created hidden, painted once with the crosshair,
// placed on the look point and hidden again when Inspect ends.
class CursorSprite {
public:
    // zorderBand > 0 -> CreateWindowInBand (above the shell; needs UIAccess). autoHigh (issue
    // #269) also creates a second window in band 16; setLayer() then picks which one shows.
    bool create(int zorderBand = 0, bool autoHigh = false);
    // Automatic band (issue #269): the low window (zorderBand, as before) and a band-16 one hold
    // the same crosshair at the same place, and exactly one is ever shown. Switching invalidates
    // the paint, so the next showCrosshair() paints the incoming window, and the next show()
    // reveals it BEFORE hiding the outgoing one: no frame without a crosshair.
    bool hasHigh() const { return hwndHigh_ != nullptr; }
    SpriteLayer layer() const { return layer_; }
    void setLayer(SpriteLayer l);
    // The band the window ACTUALLY got (the cascade can refuse the request; band_window logs it).
    int usedBand() const { return usedBand_; }
    void moveTo(int desktopX, int desktopY);
    void reapplyPosition();   // re-place after a hotspot change (the crosshair centres its hotspot)
    void show();
    void hide();
    // Re-assert HWND_TOPMOST when a window has been displaced above us. The crosshair is composited
    // OUTSIDE the fullscreen magnification, so it competes in real z-order with real windows
    // (tray/context menus, notification flyouts, other always-on-top apps); without this it is
    // raised once at create() and any topmost popup that appears later stays over it. Call each
    // active tick while shown. Mirrors RenderEngine's overlayDisplaced re-assert.
    void keepOnTop();
    // Inspect mode: paint the crosshair (BuildCrosshairBGRA, the same design the render model draws)
    // and show it. The hotspot is the cross center, so moveTo() places the crosshair ON the look
    // point. Cached: repaints only on the first call after a layer switch.
    void showCrosshair();
    void destroy();
private:
    int usedBand_ = 0;
    HWND makeWindow(int band, int* usedBand);
    HWND hwndLow_ = nullptr;           // zorderBand (band 2 under UIAccess by default)
    HWND hwndHigh_ = nullptr;          // band 16, only with autoHigh
    HWND pendingHide_ = nullptr;       // the outgoing window after a switch, hidden by show()/hide()
    SpriteLayer layer_ = SpriteLayer::Low;
    void renderCrosshair();
    bool displaced() const;            // a visible, overlapping window sits above us in z-order
    static const int kSize = 64;       // base (1x) logical canvas; buffers are kSize * scale_
    int bufSize() const { return kSize * scale_; }
    HWND    hwnd_ = nullptr;           // the active window: hwndLow_ or hwndHigh_
    int     hotX_ = 0, hotY_ = 0;      // in FINAL (scaled) sprite pixels
    int     lastTargetX_ = 0, lastTargetY_ = 0;   // last moveTo target (hotspot-independent)
    bool    haveTarget_ = false;
    // Render scale, always 1: the sprite lives in desktop space and DWM's fullscreen transform
    // magnifies it with the content, so it already grows with the zoom (issue #253). The old
    // setScale() that re-rendered it larger was never wired up and was removed (issue #274).
    int     scale_ = 1;
    bool    visible_ = false;
    bool    crosshairMode_ = false;          // window currently holds the crosshair pixels
    unsigned long long lastTopmostMs_ = 0;   // last HWND_TOPMOST re-assert (throttled)
};
}
