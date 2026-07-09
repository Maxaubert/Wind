#pragma once
#include <windows.h>
namespace wind {
class CompositionPin {
public:
    bool create();
    void assert_();
    void hide();
    void destroy();
private:
    HWND hwnd_ = nullptr;
    bool visible_ = false;
};
}
