#include "transform_model.h"
#include "transform.h"   // ComputeMagTransform
#include <windows.h>
#include <chrono>
#include <cstdint>

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

// The background patch sampled around the cursor. A single BitBlt of this rect is ONE readback and
// yields ~1.5k pixels, so the glyph/paper alternation of text averages out instead of aliasing the way
// a sparse handful of GetPixel probes does (each of which is its own round-trip). That statistical
// stability is what buys the fast cadence below: a cheaper sample AND a less noisy one.
static const int kPatchW = 48;
static const int kPatchH = 32;
static const int kSampleIntervalMs = 60;   // 2 agreeing samples => ~120ms to flip ink

// Runs off the tick thread: reading the screen forces a compositor readback (stalling composition, and
// stepping the zoom glide if done per tick), so the sampling lives here and publishes a cached verdict
// instead. Every flip is still gated on a decisive luminance band AND two consecutive agreeing samples
// - otherwise the caret strobes black/white as it crosses the glyph/paper boundary of the text it sits
// in - but at kSampleIntervalMs that gate costs ~120ms rather than the ~500ms a 250ms cadence did.
void TransformModel::polaritySampler() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

    // Build the scratch surface once; per-sample work is then one BitBlt plus a linear scan.
    HDC screen = GetDC(nullptr);
    HDC memDc  = screen ? CreateCompatibleDC(screen) : nullptr;
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = kPatchW;
    bmi.bmiHeader.biHeight = -kPatchH;   // top-down DIB
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = memDc ? CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
    if (screen) ReleaseDC(nullptr, screen);
    if (!memDc || !dib || !bits) {   // no scratch surface -> no sampler; the caret keeps its default ink
        if (dib) DeleteObject(dib);
        if (memDc) DeleteDC(memDc);
        return;
    }
    HGDIOBJ oldBmp = SelectObject(memDc, dib);

    bool current = true;   // matches appliedPolarityDark_ / CursorSprite's initial polarity
    int  flipStreak = 0;
    for (;;) {
        {
            std::unique_lock<std::mutex> lk(samplerMx_);
            if (samplerCv_.wait_for(lk, std::chrono::milliseconds(kSampleIntervalMs),
                                    [this] { return samplerStop_; }))
                break;   // shutdown requested; wake immediately rather than sleeping out the interval
        }
        if (!samplePolarity_.load(std::memory_order_relaxed)) { flipStreak = 0; continue; }
        int px = sampleX_.load(std::memory_order_relaxed);
        int py = sampleY_.load(std::memory_order_relaxed);

        // Clamp the patch inside the virtual desktop: a BitBlt that runs off-screen leaves those bits
        // undefined, and undefined bits would bias the luminance verdict.
        int vx = GetSystemMetrics(SM_XVIRTUALSCREEN),  vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
        int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        int w = kPatchW < vw ? kPatchW : vw;
        int h = kPatchH < vh ? kPatchH : vh;
        if (w <= 0 || h <= 0) continue;
        int left = px - w / 2, top = py - h / 2;
        if (left < vx) left = vx; else if (left + w > vx + vw) left = vx + vw - w;
        if (top  < vy) top  = vy; else if (top  + h > vy + vh) top  = vy + vh - h;

        HDC sdc = GetDC(nullptr);
        if (!sdc) continue;
        // Deliberately NO CAPTUREBLT: that flag is what folds layered windows into the result, so
        // omitting it excludes our own caret sprite and we cannot feed the chosen ink back into the
        // verdict (which would oscillate). Everything we want to measure is unlayered desktop content.
        BOOL ok = BitBlt(memDc, 0, 0, w, h, sdc, left, top, SRCCOPY);
        ReleaseDC(nullptr, sdc);
        if (!ok) continue;

        const uint32_t* rows = static_cast<const uint32_t*>(bits);
        unsigned long long lum = 0;
        int n = 0;
        for (int row = 0; row < h; ++row) {
            const uint32_t* p = rows + static_cast<size_t>(row) * kPatchW;   // stride is the full DIB width
            for (int col = 0; col < w; ++col) {
                uint32_t c = p[col];   // BI_RGB 32bpp little-endian BGRA -> 0xxxRRGGBB
                unsigned r = (c >> 16) & 0xFFu, g = (c >> 8) & 0xFFu, b = c & 0xFFu;
                lum += (r * 299u + g * 587u + b * 114u) / 1000u;   // Rec.601 luma
                ++n;
            }
        }
        if (n == 0) continue;
        int avg = static_cast<int>(lum / static_cast<unsigned>(n));
        bool decisive = avg >= 156 || avg <= 100;        // mid-grey is ambiguous: keep the current ink
        bool wanted = decisive ? (avg >= 156) : current; // light background -> dark ink
        if (wanted == current) { flipStreak = 0; continue; }
        if (++flipStreak >= 2) {
            flipStreak = 0;
            current = wanted;
            polarityWanted_.store(wanted ? 1 : 0, std::memory_order_relaxed);
        }
    }

    SelectObject(memDc, oldBmp);
    DeleteObject(dib);
    DeleteDC(memDc);
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
    } else if (useSprite_ && sprite_) {
        // cursorVisibility=never, or the hide-cursor hotkey. The block above is what MOVES the sprite,
        // so skipping it is not enough: hideSystemCursor(true) already showed the sprite at activation
        // and it would freeze on screen, visible and no longer tracking. Hide it explicitly. hide() is
        // idempotent, and the real cursor stays blanked (CursorBlanker is independent of this flag),
        // so nothing unmagnified reappears; zoom-out restores it via hideSystemCursor(false).
        sprite_->hide();
        samplePolarity_.store(false, std::memory_order_relaxed);
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
