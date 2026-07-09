#pragma once
#include <windows.h>
#include <unordered_map>
namespace wind {
class CursorSprite {
public:
    enum class ShapeStatus { Rendered, Hidden, Unsupported };
    explicit CursorSprite(const std::unordered_map<HCURSOR, HCURSOR>& originals) : originals_(originals) {}
    bool create(int zorderBand = 0);   // >0 -> CreateWindowInBand (above the shell; needs UIAccess)
    ShapeStatus refreshShape();
    void moveTo(int desktopX, int desktopY);
    void show();
    void hide();
    void destroy();
private:
    void renderMaskShape();
    static const int kSize = 64;
    const std::unordered_map<HCURSOR, HCURSOR>& originals_;
    HWND    hwnd_ = nullptr;
    HCURSOR lastCursor_ = nullptr;
    ShapeStatus lastVerdict_ = ShapeStatus::Hidden;
    HICON   iconCopy_ = nullptr;
    int     hotX_ = 0, hotY_ = 0;
    bool    visible_ = false;
};
}
