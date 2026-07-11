#pragma once
#include <windows.h>
#include <unordered_map>
namespace wind {
class CursorSprite {
public:
    // Rendered = the sprite window now shows the current cursor shape (standard shapes come from the
    // pre-blank originals map; app-loaded private handles render live). Hidden = the cursor is
    // suppressed/hidden or its shape could not be captured this tick - show nothing.
    enum class ShapeStatus { Rendered, Hidden };
    explicit CursorSprite(const std::unordered_map<HCURSOR, HCURSOR>& originals) : originals_(originals) {}
    bool create(int zorderBand = 0);   // >0 -> CreateWindowInBand (above the shell; needs UIAccess)
    ShapeStatus refreshShape();
    void moveTo(int desktopX, int desktopY);
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
    void destroy();
private:
    void renderMaskShape();
    void renderCrosshair();
    bool displaced() const;            // a visible, overlapping window sits above us in z-order
    static const int kSize = 64;
    const std::unordered_map<HCURSOR, HCURSOR>& originals_;
    HWND    hwnd_ = nullptr;
    HCURSOR lastCursor_ = nullptr;
    ShapeStatus lastVerdict_ = ShapeStatus::Hidden;
    HICON   iconCopy_ = nullptr;
    int     hotX_ = 0, hotY_ = 0;
    bool    visible_ = false;
    bool    crosshairMode_ = false;          // window currently holds the crosshair pixels
    unsigned long long lastTopmostMs_ = 0;   // last HWND_TOPMOST re-assert (throttled)
};
}
