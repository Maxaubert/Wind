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

    void suspendMagnifier();
    void resumeMagnifier();

    bool ready_ = false;
    double lastLevel_ = 1.0;      // ramp detection: UNCLAMPED level seen by the previous present()
                                  //   (unclamped so a hold past Magnifier's 16x ceiling still
                                  //   reads as ramping, not as a premature settle/handoff)
    unsigned long long lastSetMs_ = 0;   // transform-write throttle (see kSetIntervalMs)
    // Phases: Idle (Magnifier owns steady state), Ramping (Magnify.exe SUSPENDED, we own the
    // transform), Handoff (resumed; guard-assert the settled transform for a few ticks around
    // the silent registry sync - see present() for the measured timing rationale).
    enum class Phase { Idle, Ramping, Handoff };
    Phase  phase_ = Phase::Idle;
    int    handoffTicks_ = 0;
    // Ramp-segment state. The actual transform is read ONCE at segment start and then tracked in
    // floats: never re-read mid-ramp, so nothing foreign can be ADOPTED as our baseline, and the
    // anchor keeps sub-pixel continuity instead of round-tripping through integer offsets.
    double curLvl_ = 1.0, curOx_ = 0.0, curOy_ = 0.0;
    // Suspension bookkeeping (Magnify.exe is a UIAccess process: opening it for suspend/resume
    // works from the deployed UIAccess build; the dev build falls back to re-assert-only).
    void*  hProc_ = nullptr;
    unsigned long pid_ = 0;
    bool   suspended_ = false;
    bool   suspendWarned_ = false;
    int    lastRegPct_ = 100;     // last value written to the registry: a same-value write fires
                                  //   NO notification, so routes that need Magnifier to act must
                                  //   check this first
    unsigned long long lastLaunchMs_ = 0;   // relaunch backoff (user may close Magnifier manually)
    std::wstring backupPath_;     // one-shot registry snapshot (restore on shutdown)
};

// Safety net for abnormal exits (crash filter, atexit): a suspended Magnify.exe must NEVER be
// left behind - the user's system magnifier would be frozen until they kill it themselves.
// Idempotent, callable from any thread, no-op when nothing is suspended.
void MagnifyEmergencyResume();
}
