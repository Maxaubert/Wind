#pragma once
namespace wind {
class MagHost {
public:
    bool initialize();
    bool setTransform(float zoom, int offX, int offY, int tx, int ty, bool fastPan);
    // True while the private sub-pixel pan channel is loaded and has not failed (diagnostics).
    bool privateActive() const { return !privateBroken_ && setMagDesktop_ != nullptr; }
    void shutdown();
private:
    bool initialized_ = false;
    bool privateBroken_ = false;
    int  (__stdcall* setMagDesktop_)(double, int, int) = nullptr;
};
}
