#pragma once
namespace wind {
class MagnifierEngine {
public:
    bool initialize();                                  // MagInitialize
    void setTransform(double level, int xOffset, int yOffset);
    void shutdown();                                    // reset to 1x then MagUninitialize
    bool ready() const { return ready_; }
private:
    bool ready_ = false;
    bool inputTransformOn_ = false;   // MagSetInputTransform currently enabled
};
}
