#pragma once
#include <atomic>
namespace wind {
// Holds input state shared between the hook/raw-input callbacks and the tick thread.
struct InputState {
    std::atomic<int>  rawDx{0};      // summed since last drain
    std::atomic<int>  rawDy{0};
    std::atomic<bool> inHeld{false}; // zoom-in side button physically down
    std::atomic<bool> outHeld{false};
    std::atomic<bool> recenter{false};
};

class InputRouter {
public:
    // inButtonId/outButtonId: 1 = XBUTTON1, 2 = XBUTTON2. swallow: block the buttons
    // from reaching other apps while running.
    bool start(int inButtonId, int outButtonId, bool swallow);
    void stop();
    InputState& state() { return state_; }
    // Atomically read and zero the accumulated raw deltas.
    void drainRaw(int& dx, int& dy);
    // Map an XBUTTON id (1 = XBUTTON1, 2 = XBUTTON2) to the in/out held state, using the
    // configured zoom buttons. Shared by the WH_MOUSE_LL hook and main's WM_INPUT path.
    void setButtonState(int xbuttonId, bool down);
    // Whether the id is one of the configured zoom buttons (used to decide swallowing).
    bool isZoomButton(int xbuttonId) const;
    // Whether the hook should swallow the configured zoom buttons (set in start()).
    bool swallowEnabled() const { return swallow_; }
    // True when the LL mouse hook is installed (the normal build). When true the hook is the SOLE
    // authority for side-button held state; main's WM_INPUT path must NOT also write button state
    // (Raw Input still delivers the transition even though the hook swallows the legacy message, so
    // both writing would race/double-count). WM_INPUT button writes are only the WIND_NOHOOK fallback.
    bool hookActive() const { return hookActive_.load(std::memory_order_relaxed); }
    // Live-rebind the configured zoom buttons (called from the tick thread on hot-reload).
    // Atomic so the hook thread's reads in setButtonState/isZoomButton stay race-free, and the
    // held flags are cleared so a stale press of the previous button does not stick.
    void setButtons(int inButtonId, int outButtonId);
private:
    InputState state_;
    std::atomic<int> inButtonId_{2};   // 1 = XBUTTON1, 2 = XBUTTON2 (set in start())
    std::atomic<int> outButtonId_{1};
    bool swallow_ = true;
    std::atomic<bool> hookActive_{false};   // true once the LL hook is installed (not WIND_NOHOOK)
};

// Called from main.cpp's WM_INPUT handler with decoded relative mouse deltas.
void AccumulateRaw(InputRouter& r, int dx, int dy);
}
