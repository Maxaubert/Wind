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

// --- MpoGhost (issue #191) ----------------------------------------------------------------

static const wchar_t* kGhostClass = L"WindMpoGhost";

bool MpoGhost::create(int x, int y, int w, int h) {
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = hInst;
    wc.lpszClassName = kGhostClass;
    RegisterClassExW(&wc);   // benign if already registered

    const DWORD exStyle = WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT
                        | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;
    x_ = x; y_ = y; w_ = w; h_ = h;
    hwnd_ = CreateWindowExW(exStyle, kGhostClass, L"WindMpoGhost", WS_POPUP,
                            x, y, w, h, nullptr, nullptr, hInst, nullptr);
    if (!hwnd_) return false;
    SetLayeredWindowAttributes(hwnd_, 0, 1, LWA_ALPHA);   // alpha 1, never 0 (DWM drops 0)
    SetWindowDisplayAffinity(hwnd_, WDA_EXCLUDEFROMCAPTURE);   // invisible to DDA/screenshots
    return true;
}

void MpoGhost::assert_() {
    if (!hwnd_) return;
    // CALM re-assert: an unconditional SetWindowPos(TOPMOST) is a synchronous DWM z-order
    // transaction that hitches whatever is underneath - the same law that made
    // CursorSprite::keepOnTop and the render overlay read-first. READ the state and transact
    // only when it actually regressed (hidden, moved, or TOPMOST stripped). The demotion needs
    // only "shown fullscreen above the target", which WS_EX_TOPMOST maintains on its own, so
    // the steady-state cadence costs three cheap reads and no transaction at all.
    if (visible_ && IsWindowVisible(hwnd_)) {
        RECT rc{};
        const bool rectOk = GetWindowRect(hwnd_, &rc) && rc.left == x_ && rc.top == y_ &&
                            rc.right == x_ + w_ && rc.bottom == y_ + h_;
        const bool topmost = (GetWindowLongPtrW(hwnd_, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
        if (rectOk && topmost) return;
    }
    SetWindowPos(hwnd_, HWND_TOPMOST, x_, y_, w_, h_, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    visible_ = true;
    // Any transacted assert re-arms the settle clock (first show or a repair): the plane
    // demotion may need to happen again, and settled() must never claim stale evidence.
    shownAtMs_ = GetTickCount64();
}

void MpoGhost::hide() {
    if (hwnd_ && visible_) { ShowWindow(hwnd_, SW_HIDE); visible_ = false; shownAtMs_ = 0; }
}

void MpoGhost::destroy() {
    hide();
    if (hwnd_) { DestroyWindow(hwnd_); hwnd_ = nullptr; }
}

bool MpoGhost::settled(unsigned long long nowMs) const {
    if (!hwnd_ || !visible_ || shownAtMs_ == 0) return false;
    if (nowMs - shownAtMs_ < 350) return false;      // plane-demotion settle window
    if (!IsWindowVisible(hwnd_)) return false;       // verify, never assume (fail-closed)
    RECT rc{};
    if (!GetWindowRect(hwnd_, &rc)) return false;
    return rc.left == x_ && rc.top == y_ && rc.right == x_ + w_ && rc.bottom == y_ + h_;
}
}
