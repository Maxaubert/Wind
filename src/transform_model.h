#pragma once
#include "magnifier_model.h"
#include "mag_host.h"
#include "comp_pin.h"
#include "cursor_blanker.h"
#include "cursor_sprite.h"
#include <memory>
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
    unsigned long long lastChangeMs_ = 0;            // when the transform last REALLY changed
    bool haveLastClick_ = false;                     // dedup the OS-cursor recenter (SetCursorPos)
    int  lastClickX_ = 0, lastClickY_ = 0;
};
}
