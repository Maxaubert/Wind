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
    void lastWeld(int& x, int& y) const override { x = lastWeldX_; y = lastWeldY_; }
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
    unsigned long long lastDiagMs_ = 0;             // TEMP diagnostic throttle (issue #139)
    int  lastWeldX_ = 0, lastWeldY_ = 0;             // where present() welded the OS cursor (T(C))
};
}
