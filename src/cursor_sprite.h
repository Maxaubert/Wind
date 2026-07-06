#pragma once
#include <windows.h>
#include <unordered_map>
namespace wind {
class CursorSprite {
public:
    enum class ShapeStatus { Rendered, Hidden, Unsupported };
    explicit CursorSprite(const std::unordered_map<HCURSOR, HCURSOR>& originals) : originals_(originals) {}
    bool create();
    bool needsPolarity() const { return needsPolarity_; }
    ShapeStatus refreshShape();
    void setPolarity(bool darkCursor);
    void moveTo(int desktopX, int desktopY);
    void show();
    void hide();
    void destroy();
private:
    void renderMaskShape(bool darkCursor);
    static const int kSize = 64;
    const std::unordered_map<HCURSOR, HCURSOR>& originals_;
    HWND    hwnd_ = nullptr;
    HCURSOR lastCursor_ = nullptr;
    ShapeStatus lastVerdict_ = ShapeStatus::Hidden;
    HICON   iconCopy_ = nullptr;
    int     hotX_ = 0, hotY_ = 0;
    bool    visible_ = false;
    bool    needsPolarity_ = false;
    bool    lastPolarityDark_ = true;
};
}
