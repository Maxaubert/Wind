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
    TransformModel(bool fastPan, bool smoothPan, bool useSprite, int zorderBand)
        : fastPan_(fastPan), smoothPan_(smoothPan), useSprite_(useSprite), zorderBand_(zorderBand) {}
    bool initialize(const MonitorTarget& monitor) override;
    void shutdown() override;
    bool ready() const override { return ready_; }
    void hideSystemCursor(bool hide) override;
    void setActive(bool active) override;
    void onActivate() override {}                 // no capture to prime
    void idleTick() override;                     // expire the rest-warm (see magnifier_model.h)
    void present(const MapResult& r, double level, const Config& cfg,
                 const MonitorTarget& mon, const PresentExtras& ex) override;
    bool coversShell() const override { return false; }
private:
    bool fastPan_, smoothPan_, useSprite_;
    int  zorderBand_;                                // sprite z-band (above the shell); needs UIAccess
    bool ready_ = false;
    bool active_ = false;
    MonitorTarget mon_{};
    MagHost host_;
    CompositionPin pin_;
    std::unique_ptr<CursorBlanker> blanker_;
    std::unique_ptr<CursorSprite> sprite_;
    unsigned long long lastPinAssertMs_ = 0;
    int  keepAliveTick_ = 0;                         // alternates the tx keep-alive (issue #148)
    bool inputXformWarned_ = false;                  // one-shot warn when MagSetInputTransform fails
    bool lastInputXformOn_ = false;                  // knob edge: disable the OS transform on 1->0
    int  hiRampTick_ = 0;                            // >8x ramp divisor (TDR guard, issue #148)
    int  lastOffX_ = 0, lastOffY_ = 0, lastTxX_ = 0, lastTxY_ = 0;   // last applied transform
    double lastLevel_ = 0.0;
    double lastRequestedLevel_ = 0.0;   // detect "ramp stopped" so the final level always lands
    double sessionMaxLevel_ = 0.0;      // logged at teardown: scripted-run engagement proof
    unsigned long long restWarmMs_ = 0; // when the rest-warm started (0 = parked / not warm)
    bool magUp_ = false;                // magnification context alive (lazy; see ensureMag)
    bool ensureMag();                   // bring the context up on demand (first write of a zoom)
    void teardownMag();                 // MagUninitialize - leaves DWM's magnification mode
    unsigned long long lastChangeMs_ = 0;            // when the transform last REALLY changed
    int  lastSpriteX_ = INT_MIN, lastSpriteY_ = INT_MIN;   // dedup the game-session sprite move
    // Transform WRITE path (issue #148 hitch hunt): the DWM call can block for tens of ms, which
    // stalls our whole tick (measured: 34-86ms tick stalls coinciding with 31-59ms game frames).
    // asyncTx=1 hands the write to a dedicated thread with latest-value coalescing so the tick
    // never waits. Instrumentation logs per-second max/avg write time either way.
    void writeTransform(float lvl, int offX, int offY, int tx, int ty, bool fast, bool unusedAsync);
    void noteWrite(double ms, bool ok);
    std::mutex statMx_;
    unsigned long long statLogMs_ = 0;
    double statMaxMs_ = 0.0, statSumMs_ = 0.0;
    int    statCount_ = 0, statFails_ = 0, statOver5_ = 0;
};
}
