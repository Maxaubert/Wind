#pragma once
#include "magnifier_model.h"
#include "mag_host.h"
#include "comp_pin.h"
#include "cursor_blanker.h"
#include "cursor_sprite.h"
#include <memory>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
namespace wind {
class TransformModel : public IMagnifierModel {
public:
    TransformModel(bool fastPan, bool smoothPan, bool useSprite, int zorderBand)
        : fastPan_(fastPan), smoothPan_(smoothPan), useSprite_(useSprite), zorderBand_(zorderBand) {}
    // Safety net: a joinable std::thread destructor calls std::terminate. shutdown() normally joins,
    // but a path that destroys the model without it (self-tests) must not take the process down.
    ~TransformModel() override { stopSampler(); }
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
    // Background thread body: samples desktop luminance under the cursor and publishes the wanted
    // caret ink polarity. Never touches sprite_ - the tick thread owns it and applies the verdict.
    void polaritySampler();
    void stopSampler();          // idempotent: signal + join the sampler thread if it is running

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

    // --- caret ink polarity (mask/inverting cursors only) ---
    // The real caret is an XOR cursor with no colour of its own, so the sprite that replaces it must
    // pick an ink. A background thread samples the desktop under the cursor and publishes the verdict;
    // present() (tick thread) reads it and re-renders the mask shape only when it actually flips.
    std::thread             samplerThread_;
    std::mutex              samplerMx_;
    std::condition_variable samplerCv_;
    bool                    samplerStop_ = false;      // guarded by samplerMx_
    std::atomic<bool>       samplePolarity_{false};    // tick thread: is a mask cursor on screen?
    std::atomic<int>        sampleX_{0}, sampleY_{0};  // tick thread: where to sample (desktop px)
    std::atomic<int>        polarityWanted_{1};        // sampler: 1 = dark ink (light background)
    bool                    appliedPolarityDark_ = true;   // matches CursorSprite's initial polarity
};
}
