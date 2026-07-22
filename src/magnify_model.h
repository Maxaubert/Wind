#pragma once
#include "magnifier_model.h"
#include <string>
namespace wind {
// Drives the NATIVE Windows Magnifier (Magnify.exe) with Wind's controls. The DRM-safe model:
// the DWM fullscreen transform magnifies protected video (Netflix etc.) that blanks under the
// render model's Desktop Duplication capture, and Magnifier owns steady-state view/cursor
// tracking (follow-the-mouse panning, input remap), which it does better than our removed
// transform model ever did.
//
// HYBRID control (measured; rationale + gotchas in magnify_level.h): while the level is actively
// RAMPING, Wind sets the fullscreen transform directly each tick (MagSetFullscreenTransform,
// cursor-anchored) - glass smooth at Wind's configured zoom speed. When the ramp settles, ONE
// registry write hands the level to Magnifier (a visual no-op, since the actual transform
// already matches) and its native panning takes over. Big single-tick jumps (quick zoom) are
// routed through the registry instead, which buys Magnifier's ~280 ms eased animation for free.
class MagnifyModel : public IMagnifierModel {
public:
    bool initialize(const MonitorTarget& monitor) override;
    void shutdown() override;
    bool ready() const override { return ready_; }
    void hideSystemCursor(bool) override {}       // Magnifier draws and follows the cursor itself
    void setActive(bool active) override;
    void onActivate() override {}                 // no capture to prime
    void present(const MapResult& r, double level, const Config& cfg,
                 const MonitorTarget& mon, const PresentExtras& ex) override;
    bool coversShell() const override { return true; }   // Magnifier magnifies the shell too
    bool supportsInspect() const override { return false; }
private:
    bool magnifierRunning() const;
    void launchMagnifier();
    void writeRegistryPct(int pct);               // tracked write (see lastRegPct_)

    bool ready_ = false;
    double lastLevel_ = 1.0;      // ramp detection: level seen by the previous present()
    bool   synced_ = false;       // steady level has been handed off to Magnifier
    int    lastRegPct_ = 100;     // last value written to the registry: a same-value write fires
                                  //   NO notification, so routes that need Magnifier to act must
                                  //   check this first
    unsigned long long lastLaunchMs_ = 0;   // relaunch backoff (user may close Magnifier manually)
    std::wstring backupPath_;     // one-shot registry snapshot (restore on shutdown)
};
}
