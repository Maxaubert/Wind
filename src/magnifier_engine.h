#pragma once
namespace wind {
class MagnifierEngine {
public:
    bool initialize();                                  // MagInitialize
    // Updates the visual fullscreen transform every call (cheap). The input-routing transform
    // (MagSetInputTransform - a UIAccess system-wide input remap, comparatively expensive) is
    // refreshed only when syncInput is true, OR the first time we go past 1x (so clicks map from
    // the first zoomed frame). Callers throttle syncInput during a pan and force it true on settle,
    // so we never spam the heavy input remap at the full loop rate (that caused in-game frame spikes).
    void setTransform(double level, int xOffset, int yOffset, bool syncInput = true);
    void shutdown();                                    // reset to 1x then MagUninitialize
    bool ready() const { return ready_; }
private:
    bool ready_ = false;
    bool inputTransformOn_ = false;   // MagSetInputTransform currently enabled
};
}
