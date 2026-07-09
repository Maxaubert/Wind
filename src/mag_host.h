#pragma once
namespace wind {
class MagHost {
public:
    bool initialize();
    bool setTransform(float zoom, int offX, int offY, int tx, int ty, bool fastPan);
    // Suppress EVERY cursor shape while magnified. The real cursor is NOT magnified (unlike the
    // sprite, an UpdateLayeredWindow window, which rides the transform), so it is drawn at the
    // unmagnified click point C while the item a click at C selects is drawn at T(C). Aiming at the
    // real cursor is what caused the click drift. CursorBlanker's SetSystemCursor only covers the
    // standard shared cursors, so app-custom shapes (browsers) leak through without this.
    void showSystemCursor(bool show);
    void shutdown();
private:
    bool initialized_ = false;
    bool privateBroken_ = false;
    int  (__stdcall* setMagDesktop_)(double, int, int) = nullptr;
};
}
