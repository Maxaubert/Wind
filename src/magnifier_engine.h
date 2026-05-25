#pragma once
namespace wind {
class MagnifierEngine {
public:
    bool initialize();                                  // MagInitialize
    void setTransform(double level, int xOffset, int yOffset);
    void shutdown();                                    // reset to 1x then MagUninitialize
    bool ready() const { return ready_; }
    // True if the most recent attempt to enable input routing succeeded. False means
    // MagSetInputTransform was rejected (typically no UIAccess) - clicks will misalign.
    bool inputTransformOk() const { return inputTransformOk_; }
private:
    bool ready_ = false;
    bool inputTransformOn_ = false;   // MagSetInputTransform currently enabled
    bool inputTransformOk_ = false;   // last enable attempt returned success
};
}
