#pragma once
#include "magnifier_model.h"
#include <string>
namespace wind {
// Drives the NATIVE Windows Magnifier (Magnify.exe) like a user would - nothing more. The
// DRM-safe model: Magnifier's DWM fullscreen transform magnifies protected video (Netflix etc.)
// that blanks under the render model's Desktop Duplication capture.
//
// FINAL design after an extensive measured dead-end ledger (see the spec's amendments and
// CLAUDE.md): Wind holds NO zoom state and never touches the transform or the live registry.
// While a zoom button is held, it injects Ctrl+Alt+wheel notches (Magnifier's own wheel-zoom
// shortcut) at a measured-safe cadence; Magnifier does everything else natively - stepping by
// the user's own ZoomIncrement, easing each notch, panning, cursor. Measured: notches at 60 ms
// register 1:1 (no drops, unlike Win+Plus chord bursts) and settle ~150 ms after the last one.
// Injected Win+wheel is INERT - Ctrl+Alt+wheel is the real channel.
class MagnifyModel : public IMagnifierModel {
public:
    bool initialize(const MonitorTarget& monitor) override;
    void shutdown() override;
    bool ready() const override { return ready_; }
    void hideSystemCursor(bool) override {}       // Magnifier draws and follows the cursor itself
    void setActive(bool) override {}              // level machinery is bypassed (selfDrivenZoom)
    void onActivate() override {}                 // no capture to prime
    void present(const MapResult&, double, const Config&,
                 const MonitorTarget&, const PresentExtras&) override {}   // never active
    bool coversShell() const override { return true; }   // Magnifier magnifies the shell too
    bool supportsInspect() const override { return false; }
    bool selfDrivenZoom() const override { return true; }
    void nativeZoomTick(int dir, const Config& cfg) override;
private:
    void launchMagnifier();

    bool ready_ = false;
    int  lastDir_ = 0;                      // for start/stop logging only
    int  lastStepPct_ = 0;                  // last ZoomIncrement written (write only on change;
                                            //   0 = not yet written, forces the first write)
    unsigned long long lastNotchMs_ = 0;    // wheel-notch cadence gate
    unsigned long long lastLaunchMs_ = 0;   // relaunch backoff (user may close Magnifier manually)
    std::wstring backupPath_;               // one-shot registry snapshot (restore on shutdown)
};
}
