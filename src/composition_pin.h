#pragma once
namespace wind {
// A cheap "composition heartbeat" for the Mag low-power engine. MagSetFullscreenTransform gates the
// focused fullscreen game's own swapchain to DWM's composite cadence; under VRR/G-Sync that cadence
// floats down (~half), which halves the game. Presenting a tiny (1x1) flip-model swapchain at the
// display refresh forces DWM to composite at full rate, so the gated game is pulled back up to full
// rate too. The 1x1 surface does no real drawing, so the GPU cost is negligible, and it only lives
// while zoomed. Opt-in via the compositePin config flag. No effect on fixed-refresh displays (the
// composite already runs at the steady rate there) beyond a harmless extra vsync'd present.
//
// Header stays Win32/D3D-free (pImpl) so it can be included widely; all COM lives in the .cpp.
class CompositionPin {
public:
    ~CompositionPin();
    bool begin();              // create the heartbeat window + device + swapchain; idempotent
    void present();            // clear + Present(1,0) at vsync; no-op until begun
    void end();                // tear everything down; idempotent
    bool active() const { return impl_ != nullptr; }
private:
    struct Impl;
    Impl* impl_ = nullptr;
};
}
