#include "transform_model.h"
#include "transform.h"   // ComputeMagTransform
#include <windows.h>
#include <chrono>

namespace wind {

bool TransformModel::initialize(const MonitorTarget& monitor) {
    mon_ = monitor;
    if (!host_.initialize()) return false;
    if (useSprite_) {
        blanker_ = std::make_unique<CursorBlanker>();
        sprite_  = std::make_unique<CursorSprite>(blanker_->originals());
        sprite_->create(zorderBand_);   // above the shell so the cursor covers the magnified taskbar
        samplerThread_ = std::thread(&TransformModel::polaritySampler, this);
    }
    if (smoothPan_) pin_.create();
    ready_ = true;
    return true;
}

// Sample offsets around the cursor. Text alternates glyph and paper pixels, so a spread grid (rather
// than the single pixel under the hotspot) is what makes the background verdict stable over a caret.
static const int kSampleOffX[3] = { -12, 0, 12 };
static const int kSampleOffY[3] = { -8, 0, 8 };

// Runs off the tick thread: GetPixel forces a compositor readback (stalling composition, and stepping
// the zoom glide if done per tick), so the sampling lives here and publishes a cached verdict instead.
// Every flip is gated on a decisive luminance band AND two consecutive agreeing samples - otherwise the
// caret strobes black/white as it crosses the glyph/paper boundary of the text it sits in.
void TransformModel::polaritySampler() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    bool current = true;   // matches appliedPolarityDark_ / CursorSprite's initial polarity
    int  flipStreak = 0;
    for (;;) {
        {
            std::unique_lock<std::mutex> lk(samplerMx_);
            if (samplerCv_.wait_for(lk, std::chrono::milliseconds(250),
                                    [this] { return samplerStop_; }))
                return;   // shutdown requested; wake immediately rather than sleeping out the 250ms
        }
        if (!samplePolarity_.load(std::memory_order_relaxed)) { flipStreak = 0; continue; }
        int x = sampleX_.load(std::memory_order_relaxed);
        int y = sampleY_.load(std::memory_order_relaxed);
        HDC dc = GetDC(nullptr);
        if (!dc) continue;
        int lum = 0, n = 0;
        for (int oy : kSampleOffY) {
            for (int ox : kSampleOffX) {
                COLORREF c = GetPixel(dc, x + ox, y + oy);
                if (c != CLR_INVALID) {   // off-screen / unreadable pixels just don't vote
                    lum += (GetRValue(c) * 299 + GetGValue(c) * 587 + GetBValue(c) * 114) / 1000;
                    n++;
                }
            }
        }
        ReleaseDC(nullptr, dc);
        if (n == 0) continue;
        int avg = lum / n;
        bool decisive = avg >= 156 || avg <= 100;        // mid-grey is ambiguous: keep the current ink
        bool wanted = decisive ? (avg >= 156) : current; // light background -> dark ink
        if (wanted == current) { flipStreak = 0; continue; }
        if (++flipStreak >= 2) {
            flipStreak = 0;
            current = wanted;
            polarityWanted_.store(wanted ? 1 : 0, std::memory_order_relaxed);
        }
    }
}

void TransformModel::hideSystemCursor(bool hide) {
    if (!useSprite_ || !blanker_) return;
    if (hide) { blanker_->blank(); if (sprite_) sprite_->show(); }
    else      { if (sprite_) sprite_->hide(); blanker_->restore(); }
}

void TransformModel::setActive(bool active) {
    active_ = active;
    if (!active) {
        host_.setTransform(1.0f, 0, 0, 0, 0, false);   // back to 1x
        pin_.hide();
        haveLastClick_ = false;   // re-warp fresh on the next activation
        samplePolarity_.store(false, std::memory_order_relaxed);   // idle the sampler while unzoomed
    }
}

void TransformModel::present(const MapResult& r, double level, const Config& cfg,
                             const MonitorTarget& mon, const PresentExtras& ex) {
    (void)cfg; (void)mon;
    MagTransform m = ComputeMagTransform(r.srcLeft, r.srcTop, level);
    host_.setTransform((float)level, m.offX, m.offY, m.txX, m.txY, fastPan_);

    // Weld the hidden OS cursor to the lens point, exactly as RenderEngine::render does. This keeps
    // the scene-locked sprite on the real click point AND keeps RunTick's warp-and-measure pan
    // tracking consistent (RunTick assumes the cursor was moved here each active tick). Deduped so an
    // idle tick injects no synthetic mouse move. Inspect freeze pins the point via ex.clickOverride;
    // otherwise clickDesktop is monitor-local, so add the monitor origin for desktop px.
    int cx = ex.clickOverride ? ex.clickDesktopX : (r.clickDesktopX + mon_.x);
    int cy = ex.clickOverride ? ex.clickDesktopY : (r.clickDesktopY + mon_.y);
    if (!haveLastClick_ || cx != lastClickX_ || cy != lastClickY_) {
        SetCursorPos(cx, cy);
        lastClickX_ = cx; lastClickY_ = cy; haveLastClick_ = true;
    }

    if (useSprite_ && sprite_ && ex.drawCursor) {
        CursorSprite::ShapeStatus st = sprite_->refreshShape();
        if (st == CursorSprite::ShapeStatus::Rendered) {
            // The transform magnifies the sprite welded to the content at the same click point.
            sprite_->moveTo(cx, cy);
            sprite_->show();
            if (sprite_->needsPolarity()) {
                // A mask/inverting cursor (the caret): it has no colour of its own, so the ink has to
                // be chosen from the background. Publish where to sample and apply the sampler thread's
                // cached verdict; setPolarity() no-ops unless the polarity actually flipped, so the
                // (cheap, memory-DC) re-render only happens on a real change.
                sampleX_.store(cx, std::memory_order_relaxed);
                sampleY_.store(cy, std::memory_order_relaxed);
                samplePolarity_.store(true, std::memory_order_relaxed);
                bool wantDark = polarityWanted_.load(std::memory_order_relaxed) == 1;
                if (wantDark != appliedPolarityDark_) {
                    appliedPolarityDark_ = wantDark;
                    sprite_->setPolarity(wantDark);
                }
            } else {
                samplePolarity_.store(false, std::memory_order_relaxed);   // ordinary coloured cursor
            }
        } else {
            sprite_->hide();   // Hidden/Unsupported: show the real (or app-custom) cursor instead
            samplePolarity_.store(false, std::memory_order_relaxed);
        }
    }

    if (smoothPan_ && level > 1.0) {
        unsigned long long now = GetTickCount64();
        if (now - lastPinAssertMs_ >= 500) { lastPinAssertMs_ = now; pin_.assert_(); }
    } else {
        pin_.hide();
    }
}

void TransformModel::stopSampler() {
    if (!samplerThread_.joinable()) return;
    { std::lock_guard<std::mutex> lk(samplerMx_); samplerStop_ = true; }
    samplerCv_.notify_all();
    samplerThread_.join();
}

void TransformModel::shutdown() {
    // Stop the sampler BEFORE tearing the sprite down. It never touches sprite_, but joining first
    // keeps the thread from outliving the object it holds a `this` pointer into.
    stopSampler();
    if (sprite_) sprite_->destroy();
    if (blanker_) blanker_->restore();
    pin_.destroy();
    host_.shutdown();
    ready_ = false;
}
}
