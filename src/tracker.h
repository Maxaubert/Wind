#pragma once
namespace wind {
// Pure tracking state. Caller samples GetCursorPos and the summed raw-input deltas
// since the last tick, then calls update() once per tick.
class Tracker {
public:
    Tracker(int screenW, int screenH, double sensitivity);
    // cursorX/Y: latest GetCursorPos. rawDx/Dy: summed WM_INPUT deltas since last call.
    void update(int cursorX, int cursorY, int rawDx, int rawDy);
    void recenter();                 // snap to screen center
    double centerX() const { return cx_; }
    double centerY() const { return cy_; }
private:
    void clamp();
    int screenW_, screenH_;
    double sensitivity_;
    double cx_, cy_;
    int lastCursorX_, lastCursorY_;
    bool haveCursor_ = false;
};
}
