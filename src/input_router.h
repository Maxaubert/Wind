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
private:
    InputState state_;
};

// Called from main.cpp's WM_INPUT handler with decoded relative mouse deltas.
void AccumulateRaw(InputRouter& r, int dx, int dy);
}
