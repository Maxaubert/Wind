#pragma once
namespace wind {
class MagnifierEngine {
public:
    bool initialize();                                  // MagInitialize
    // Updates the visual fullscreen transform every call. inputXform controls the input-routing
    // transform (MagSetInputTransform): true = remap clicks into the magnified region; false = leave
    // input un-remapped (the lens follows the raw cursor, clicks land at the real cursor position - no
    // edge dead band from the recenter feedback loop). Either way, a stale transform is torn down.
    void setTransform(double level, int xOffset, int yOffset, bool inputXform = true);
    // Force the system input transform OFF. MagSetInputTransform is system-global state that can be
    // left active by a crashed/killed process (it then confines clicks to a zoom-shrinking box - the
    // dead zone). The cursor-warp path calls this every tick so any stale transform is wiped.
    void clearInputTransform();
    void shutdown();                                    // reset to 1x then MagUninitialize
    bool ready() const { return ready_; }
private:
    bool ready_ = false;
    bool inputTransformOn_ = false;   // MagSetInputTransform currently enabled
};
}
