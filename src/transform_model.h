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
    void UpdatePanOffset(double cx, double cy, double level);   // edge-triggered pan + zoom anchor

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
    bool haveLastClick_ = false;                     // dedup the OS-cursor recenter (SetCursorPos)
    int  lastClickX_ = 0, lastClickY_ = 0;

    // --- edge-triggered pan ---
    // The view holds still while the cursor roams, and only pans when the cursor reaches a margin at
    // the screen edge. Held across ticks (unlike the render model, whose offset is a pure function of
    // the cursor). Re-anchored on every zoom change so the point under the cursor stays put.
    double offX_ = 0.0, offY_ = 0.0;
    double lastLevel_ = 1.0;
    bool   haveOff_ = false;                         // seeded on the first active tick
};
}
