#include "comp_pin.h"
namespace wind {

static const wchar_t* kPinClass = L"WindCompositionPin";

bool CompositionPin::create() {
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = hInst;
    wc.lpszClassName = kPinClass;
    RegisterClassExW(&wc);   // benign if already registered (returns 0 with ERROR_CLASS_ALREADY_EXISTS)

    const DWORD exStyle = WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT
                        | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;
    hwnd_ = CreateWindowExW(exStyle, kPinClass, L"WindPin", WS_POPUP,
                            0, 0, 1, 1, nullptr, nullptr, hInst, nullptr);
    if (!hwnd_) return false;
    // Uniform 1/255 alpha: composited (so DWM keeps the game composed) but imperceptible.
    // Never 0 - DWM drops fully transparent windows.
    SetLayeredWindowAttributes(hwnd_, 0, 1, LWA_ALPHA);
    return true;
}

void CompositionPin::assert_() {
    if (!hwnd_) return;
    SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE);
    if (!visible_) { ShowWindow(hwnd_, SW_SHOWNOACTIVATE); visible_ = true; }
}

void CompositionPin::hide() {
    if (hwnd_ && visible_) { ShowWindow(hwnd_, SW_HIDE); visible_ = false; }
}

void CompositionPin::destroy() {
    hide();
    if (hwnd_) { DestroyWindow(hwnd_); hwnd_ = nullptr; }
}
}
