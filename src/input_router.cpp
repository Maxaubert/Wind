#include "input_router.h"
#include <windows.h>
namespace wind {
static InputRouter* g_router = nullptr;
static HHOOK g_mouseHook = nullptr;

static int xbuttonIdFromHook(WPARAM wParam, LPARAM lParam) {
    auto* mi = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);
    if (wParam == WM_XBUTTONDOWN || wParam == WM_XBUTTONUP) {
        WORD hi = HIWORD(mi->mouseData); // XBUTTON1 or XBUTTON2
        return (hi == XBUTTON1) ? 1 : (hi == XBUTTON2 ? 2 : 0);
    }
    return 0;
}

// Shared by the WH_MOUSE_LL hook (below) and main's WM_INPUT path: map an XBUTTON id to held.
void InputRouter::setButtonState(int xbuttonId, bool down) {
    if (xbuttonId == inButtonId_)  state_.inHeld.store(down);
    if (xbuttonId == outButtonId_) state_.outHeld.store(down);
}
bool InputRouter::isZoomButton(int xbuttonId) const {
    return xbuttonId == inButtonId_ || xbuttonId == outButtonId_;
}

static LRESULT CALLBACK MouseProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION && g_router) {
        int id = xbuttonIdFromHook(wParam, lParam);
        bool down = (wParam == WM_XBUTTONDOWN);
        bool up   = (wParam == WM_XBUTTONUP);
        if (id != 0 && (down || up)) {
            g_router->setButtonState(id, down);
            if (g_router->swallowEnabled() && g_router->isZoomButton(id))
                return 1; // swallow so browser back/forward don't fire
        }
    }
    return CallNextHookEx(g_mouseHook, code, wParam, lParam);
}

bool InputRouter::start(int inButtonId, int outButtonId, bool swallow) {
    g_router = this; inButtonId_ = inButtonId; outButtonId_ = outButtonId; swallow_ = swallow;
    g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, MouseProc, GetModuleHandleW(nullptr), 0);
    return g_mouseHook != nullptr;
    // Raw Input registration (RIDEV_INPUTSINK) + WM_INPUT decoding live in main.cpp's
    // message-only window, which calls AccumulateRaw() with the decoded deltas.
}
void InputRouter::stop() {
    if (g_mouseHook) { UnhookWindowsHookEx(g_mouseHook); g_mouseHook = nullptr; }
    g_router = nullptr;
}
void InputRouter::drainRaw(int& dx, int& dy) {
    dx = state_.rawDx.exchange(0);
    dy = state_.rawDy.exchange(0);
}

void AccumulateRaw(InputRouter& r, int dx, int dy) {
    r.state().rawDx.fetch_add(dx);
    r.state().rawDy.fetch_add(dy);
}
}
