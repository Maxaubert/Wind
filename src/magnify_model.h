#pragma once
#include "magnifier_model.h"
#include <string>
namespace wind {
// Drives the NATIVE Windows Magnifier (Magnify.exe) with Wind's controls. The DRM-safe model:
// Magnifier uses the DWM fullscreen transform, so protected video (Netflix etc.) that blanks
// under the render model's Desktop Duplication capture magnifies fine, and the OS implementation
// owns the view, cursor tracking, and stability. Wind's job reduces to syncing Magnifier's
// stepped level to the smooth ZoomController level by injecting Win+Plus/Win+Minus chords
// (Magnifier's only live external control), budgeted per tick. See
// docs/superpowers/specs/2026-07-22-magnify-model-design.md and src/magnify_steps.h.
class MagnifyModel : public IMagnifierModel {
public:
    explicit MagnifyModel(int stepPct) : stepPct_(stepPct) {}
    bool initialize(const MonitorTarget& monitor) override;
    void shutdown() override;
    bool ready() const override { return ready_; }
    void hideSystemCursor(bool) override {}       // Magnifier draws and follows the cursor itself
    void setActive(bool active) override;
    void onActivate() override {}                 // no capture to prime
    void present(const MapResult& r, double level, const Config& cfg,
                 const MonitorTarget& mon, const PresentExtras& ex) override;
    bool coversShell() const override { return true; }   // Magnifier magnifies the shell too
private:
    void injectZoomChord(bool zoomIn);
    void syncToTarget(int targetSteps, int budget);
    bool magnifierRunning() const;
    void launchMagnifier();
    int  readMagnificationPct() const;            // registry; -1 if unreadable

    int  stepPct_;
    bool ready_ = false;
    bool launched_ = false;       // we have started Magnify.exe this session
    int  currentSteps_ = 0;       // our view of Magnifier's level, in stepPct_ increments above 100%
    unsigned long long lastLaunchMs_ = 0;   // relaunch backoff (user may close Magnifier manually)
    unsigned long long lastResyncMs_ = 0;   // throttle for adopting the registry level when idle
    std::wstring backupPath_;     // one-shot registry snapshot (restore on shutdown)
};
}
