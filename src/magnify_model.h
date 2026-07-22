#pragma once
#include "magnifier_model.h"
#include <string>
namespace wind {
// Drives the NATIVE Windows Magnifier (Magnify.exe) with Wind's controls. The DRM-safe model:
// Magnifier uses the DWM fullscreen transform, so protected video (Netflix etc.) that blanks
// under the render model's Desktop Duplication capture magnifies fine, and the OS implementation
// owns the view, cursor tracking, and stability.
//
// Control channel (measured, see magnify_level.h): Magnifier watches the registry Magnification
// value LIVE and eases to it smoothly with exact fidelity for arbitrary integer percents. Wind
// writes the ramped ZoomController level whenever the integer percent changes, so hold-to-zoom
// runs at Wind's configured speed, continuously - no steps, no keystroke injection. (Injected
// Win+Plus chords were the first attempt and are a dead end: Magnifier drops roughly half of a
// rapid burst and animates each survivor, which lagged the ramp and kept zooming after release.)
// Spec: docs/superpowers/specs/2026-07-22-magnify-model-design.md (amended).
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

    bool ready_ = false;
    int  lastWrittenPct_ = 100;   // last percent written to the registry (write only on change)
    unsigned long long lastLaunchMs_ = 0;   // relaunch backoff (user may close Magnifier manually)
    std::wstring backupPath_;     // one-shot registry snapshot (restore on shutdown)
};
}
