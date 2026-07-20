#pragma once
#include "magnifier_model.h"

namespace wind {

// cursorVisibility config string -> render param: 0 = auto, 1 = always, 2 = never.
int CursorModeFromCfg(const Config& c);

// Fill a RenderFrameParams from the mapper result + config for the given monitor and zoom level
// (the normal live-tick interpretation). The self-test harnesses call this, then override only
// the few fields they deliberately differ on (cursorMode, vsync).
void FillRenderParams(RenderFrameParams& p, const MapResult& r, const Config& cfg,
                      const MonitorTarget& mon, double level);

// Adapts the existing RenderEngine (DXGI capture + D3D11 overlay) to IMagnifierModel, so the
// render path behaves identically while RunTick moves behind the interface (Task 4).
class RenderModel : public IMagnifierModel {
public:
    explicit RenderModel(int zorderBand, bool hdrTonemap);
    bool initialize(const MonitorTarget& m) override;
    void shutdown() override;
    bool ready() const override;
    void hideSystemCursor(bool hide) override;
    void setActive(bool active) override;
    void onActivate() override;
    bool retarget(const MonitorTarget& m) override;
    void present(const MapResult& r, double level, const Config& cfg,
                 const MonitorTarget& mon, const PresentExtras& ex) override;
    bool coversShell() const override;
    RenderEngine& engine();   // escape hatch for render-only main-loop code (device-lost, priming, selftest)
    bool deviceLost() const;  // forwarded (main loop calls this)
    bool recoverDeviceLost();
    void primeReveal();
    bool frameCompositedSincePrime() const;
    void invalidateCapture();
private:
    RenderEngine engine_;
    int  zorderBand_;
    bool hdrTonemap_;
    bool primed_ = false;
};
}
