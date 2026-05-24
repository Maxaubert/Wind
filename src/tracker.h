#pragma once
namespace wind {
// Pure tracking state. Caller samples GetCursorPos and the summed raw-input deltas
// since the last tick, then calls update() once per tick.
//
// Two modes chosen by a hysteresis lock detector:
//   Free   - OS cursor moving normally -> lens follows GetCursorPos.
//   Locked - OS cursor frozen (a game hid/clipped/locked it) while the hand keeps
//            moving (raw deltas arrive) -> lens pans by raw deltas. Wind's core feature.
//
// A *single* tick that sees an unchanged GetCursorPos while raw deltas arrive is NOT a
// lock: during free movement that is a sampling alias, and integrating it makes the lens
// jump off-centre then snap back (visible flicker). The lock engages only after
// kLockEngageTicks consecutive frozen-cursor-with-raw ticks and disengages immediately
// when the OS cursor moves again.
class Tracker {
public:
    // Consecutive frozen-cursor + raw ticks needed to treat the cursor as locked. At a
    // 60-144 Hz tick this is ~40-100 ms: far longer than any free-movement sampling
    // alias, and a one-time imperceptible delay when a real game lock engages.
    static constexpr int kLockEngageTicks = 6;

    Tracker(int screenW, int screenH, double sensitivity);
    // cursorX/Y: latest GetCursorPos. rawDx/Dy: summed WM_INPUT deltas since last call.
    void update(int cursorX, int cursorY, int rawDx, int rawDy);
    void recenter();                 // snap to screen center
    double centerX() const { return cx_; }
    double centerY() const { return cy_; }
    bool   locked()  const { return locked_; }   // exposed for diagnostics/tests
private:
    void clamp();
    int screenW_, screenH_;
    double sensitivity_;
    double cx_, cy_;
    int lastCursorX_, lastCursorY_;
    bool haveCursor_ = false;
    bool locked_ = false;
    int  stationaryRawTicks_ = 0;
};
}
