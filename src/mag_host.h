#pragma once
namespace wind {
class MagHost {
public:
    bool initialize();
    bool setTransform(float zoom, int offX, int offY, int tx, int ty, bool fastPan);
    void shutdown();
private:
    bool initialized_ = false;
    bool privateBroken_ = false;
    int  (__stdcall* setMagDesktop_)(double, int, int) = nullptr;
};
}
