#pragma once
namespace wind {
// Pure on/off toggle for "Inspect mode". No <windows.h> (compiles into the desktop-free WIND_TESTS
// build). Inspect mode is now just a crosshair-cursor toggle: the tick (main.cpp) swaps the system
// cursor to a crosshair while locked() is true and restores the normal cursor when it is false. This
// class only owns the boolean toggle state.
class CursorLockController {
public:
    // Rising edge of the bound toggle key: flip the crosshair on/off.
    void toggle();
    // Back to free (crosshair off). Kept for teardown/safety callers.
    void reset();

    bool locked() const { return locked_; }
private:
    bool locked_ = false;
};
}
