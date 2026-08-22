#pragma once
#include "magnifier_model.h"
#include "mag_host.h"
#include "comp_pin.h"
#include "cursor_blanker.h"
#include "cursor_sprite.h"
#include <memory>
#include <thread>
#include <mutex>
#include <condition_variable>
namespace wind {
class TransformModel : public IMagnifierModel {
public:
    TransformModel(bool fastPan, bool smoothPan, bool useSprite, int zorderBand,
                   bool spriteBand16 = false)
        : fastPan_(fastPan), smoothPan_(smoothPan), useSprite_(useSprite),
          zorderBand_(zorderBand), spriteBand16_(spriteBand16) {}
    bool initialize(const MonitorTarget& monitor) override;
    void shutdown() override;
    bool ready() const override { return ready_; }
    void hideSystemCursor(bool hide) override;
    void setActive(bool active) override;
    void onActivate() override {}                 // no capture to prime
    void idleTick() override;                     // tears the mag context down once idle (#148)
    void setIdleReleaseMs(int ms) { idleReleaseMs_ = ms < 0 ? 0 : ms; }
    void present(const MapResult& r, double level, const Config& cfg,
                 const MonitorTarget& mon, const PresentExtras& ex) override;
    bool coversShell() const override { return false; }
    // True when THIS present actually called SetCursorPos (weld executed; not deduped, not
    // suppressed by drag-follow). RunTick's #169 measured-baseline logic reads it exactly like
    // RenderEngine::parkedLastFrame(): baseline on the weld point only when the weld really fired.
    // Written-transform readbacks for the telemetry channel (issue #227): what the tick path
    // last actually applied (the hook writer keeps its own cache and is not reflected here).
    double writtenLevel() const { return lastLevel_; }
    int    writtenTxX() const { return lastTxX_; }
    int    writtenTxY() const { return lastTxY_; }
    bool weldedLastFrame() const { return weldedLastFrame_; }
    // Whether MagSetInputTransform is usable (probed once at initialize; needs UIAccess). The
    // hybrid DESKTOP pick requires this: without the source-rect input transform, transform
    // desktop sessions have the pointer-framework hover dead zones (POINTER-HITTEST-FINDINGS.md).
    bool inputTransformAvailable() const { return inputTransformAvailable_; }
    // MPO buster (issue #191). Wanted = show the fullscreen alpha-1 ghost this session (MPO-
    // exposed game session + the mpoBuster knob); exposed = the session could overflow the
    // 16-bit plane field, so the write-site clamp applies whenever the ghost is not verifiably
    // settled. ghostSettled = the fail-closed evidence the pan-wall lift requires.
    void setMpoBusterWanted(bool wanted) { mpoBusterWanted_ = wanted; }
    void setMpoExposed(bool exposed) { mpoExposed_ = exposed; }
    bool mpoGhostSettled() const { return mpoGhost_.settled(GetTickCount64()); }
    // For the hook write path (issue #206): the hook needs the SAME host, since the runtime is
    // refcounted per process and a second one would not be the context DWM is holding.
    MagHost* magHost() { return &host_; }
private:
    bool fastPan_, smoothPan_, useSprite_;
    int  zorderBand_;                                // sprite z-band (above the shell); needs UIAccess
    bool spriteBand16_ = false;                      // P2 experiment: band-16 SCREEN-space sprite
    bool ready_ = false;
    bool active_ = false;
    MonitorTarget mon_{};
    MagHost host_;
    CompositionPin pin_;
    MpoGhost mpoGhost_;                              // MPO buster (issue #191)
    bool mpoBusterWanted_ = false;                   // show the ghost this session
    bool mpoExposed_ = false;                        // apply the 16-bit write clamp
    unsigned long long lastGhostAssertMs_ = 0;       // 500ms assert cadence
    int  appliedSampling_ = -2;                      // sampling mode DWM currently holds (-2 = unknown)
    std::unique_ptr<CursorBlanker> blanker_;
    std::unique_ptr<CursorSprite> sprite_;
    unsigned long long lastPinAssertMs_ = 0;
    int  keepAliveTick_ = 0;                         // alternates the tx keep-alive (issue #148)
    bool inputXformWarned_ = false;                  // one-shot warn when MagSetInputTransform fails
    bool lastInputXformOn_ = false;                  // knob edge: disable the OS transform on 1->0
    int  hiRampTick_ = 0;                            // >8x ramp divisor (TDR guard, issue #148)
    int  lastOffX_ = 0, lastOffY_ = 0, lastTxX_ = 0, lastTxY_ = 0;   // last applied transform
    double lastLevel_ = 0.0;
    double lastRequestedLevel_ = 0.0;
    double sessionMaxLevel_ = 0.0;      // logged at teardown: scripted-run engagement proof
    unsigned long long lastChangeMs_ = 0;            // when the transform last REALLY changed
    unsigned long long lastWriteMs_ = 0;             // when a write last actually went out (#204)
    int  lastSpriteX_ = INT_MIN, lastSpriteY_ = INT_MIN;   // dedup the game-session sprite move
    // Magnification context lifetime (issue #148). While a context is alive DWM composites
    // magnification-aware, so every cursor visibility/shape change an app makes costs a
    // re-composite: a game that toggles its pointer on middle-click hitches even at 1x
    // (measured 17 spike frames per 14 clicks after a zoom; 0 with no context, 0 while actually
    // zoomed). So the context lives only around real zoom sessions.
    bool magUp_ = false;
    bool cursorHidden_ = false;                      // we called MagShowSystemCursor(FALSE)
    bool haveLastClick_ = false;                     // dedup the per-tick cursor weld
    int  lastClickX_ = 0, lastClickY_ = 0;
    bool weldedLastFrame_ = false;                   // SetCursorPos ran in the last present()
    bool inputTransformAvailable_ = false;           // MagSetInputTransform probe (UIAccess)
    unsigned long long idleSinceMs_ = 0;             // when the last session ended (0 = none)
    int  idleReleaseMs_ = 1200;                      // cfg.txIdleReleaseMs (hot)
    bool identityParked_ = false;                    // phase 1 of the release done (see idleTick)
    unsigned long long parkedAtMs_ = 0;
    bool ensureMag();
    void teardownMag();
    void resetTransformState();                      // forget cached values across a teardown
    // Transform WRITE path (issue #148 hitch hunt): the DWM call can block for tens of ms, which
    // stalls our whole tick (measured: 34-86ms tick stalls coinciding with 31-59ms game frames).
    // asyncTx=1 hands the write to a dedicated thread with latest-value coalescing so the tick
    // never waits. Instrumentation logs per-second max/avg write time either way.
    void writeTransform(float lvl, int offX, int offY, int tx, int ty, bool fast, bool unusedAsync);
    void noteWrite(double ms, bool ok);
    void noteIxWrite(double ms, bool ok);            // input-transform publish stats (issue #189)
    void noteIxStomp();                              // foreign writer overwrote our publish (#217)
    std::mutex statMx_;
    unsigned long long statLogMs_ = 0;
    double statMaxMs_ = 0.0, statSumMs_ = 0.0;
    int    statCount_ = 0, statFails_ = 0, statOver5_ = 0;
    unsigned long long ixLogMs_ = 0;                 // input-transform publish stats (issue #189)
    double ixMaxMs_ = 0.0, ixSumMs_ = 0.0;
    int    ixCount_ = 0, ixFails_ = 0;
    int    ixTick_ = 0;                              // decimation counter (cfg.ixDecimate)
    bool   ixPending_ = false;                       // motion changed since the last publish
    // Stomp guard (issue #217): what we last VERIFIABLY published into the system input-
    // transform slot. Kept across sessions on purpose - a fresh session's first tick compares
    // the slot against the previous session's disable, so a rect stranded by a dead native
    // Magnifier is caught and overwritten immediately.
    bool   ixExpectedValid_ = false;
    bool   ixExpectedOn_ = false;
    int    ixExpL_ = 0, ixExpT_ = 0, ixExpR_ = 0, ixExpB_ = 0;
    int    ixStomps_ = 0;                            // foreign overwrites seen this log window
    bool   ixStompWarned_ = false;                   // one-shot foreign-writer warn
    int    ixDbgLogs_ = 0;                           // one-shot publish ground-truth logs (#217)
};
}
