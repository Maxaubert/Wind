#include "transform_model.h"
#include "transform.h"   // ComputeMagTransform
#include "logging.h"
#include <windows.h>
#include <magnification.h>

namespace wind {

bool TransformModel::initialize(const MonitorTarget& monitor) {
    mon_ = monitor;
    if (!host_.initialize()) return false;
    // Warm-up (issue #148): the SESSION'S first real zoom paid a ~110ms cold-start while DWM
    // built its magnification machinery. Touch it once at launch with an invisible 0.1% level
    // (sub-pixel everywhere), then reset - the first user zoom starts from a warm pipeline.
    host_.setTransform(1.001f, 0, 0, 0, 0, false);
    host_.setTransform(1.0f, 0, 0, 0, 0, false);
    if (useSprite_) {
        blanker_ = std::make_unique<CursorBlanker>();
        sprite_  = std::make_unique<CursorSprite>(blanker_->originals());
        sprite_->create(zorderBand_);   // above the shell so the cursor covers the magnified taskbar
    }
    if (smoothPan_) pin_.create();
    ready_ = true;
    return true;
}

void TransformModel::hideSystemCursor(bool hide) {
    if (!useSprite_ || !blanker_) return;
    // The blanker only blanks STANDARD cursors; a game's app-custom cursor stayed visible and
    // roamed at its raw desktop position (issue #148 field report). MagShowSystemCursor hides
    // the cursor plane wholesale - same call the render model relies on - so the sprite is the
    // only visible cursor in every app. Both are applied; both are undone.
    if (hide) { blanker_->blank(); MagShowSystemCursor(FALSE); if (sprite_) sprite_->show(); }
    else      { if (sprite_) sprite_->hide(); MagShowSystemCursor(TRUE); blanker_->restore(); }
}

void TransformModel::setActive(bool active) {
    active_ = active;
    if (!active) {
        // Rest at TRUE 1.0 (rest-warm 1.0005 REMOVED): keeping the magnification pipeline hot
        // over a running game full-time is a standing GPU-TDR contributor (field: driver resets
        // recurred even with the 12x cap), and the rest-warm measurably did not fix the entry
        // spike anyway. One ~30ms entry blip per zoom-in is the safe trade.
        host_.setTransform(1.0f, 0, 0, 0, 0, false);
        RECT full{ 0, 0, mon_.w, mon_.h };
        host_.setInputTransform(false, full, full);   // input mapping back to identity at 1x
        pin_.hide();
        haveLastClick_ = false;   // re-warp fresh on the next activation
    }
}

void TransformModel::present(const MapResult& r, double level, const Config& cfg,
                             const MonitorTarget& mon, const PresentExtras& ex) {
    (void)mon;
    // CENTERED-CURSOR geometry (issue #148 revival). The original model anchored the transform at
    // the cursor (ComputeFixedPointOffset) because a sprite AT the cursor's real position only sits
    // on correct content when T(L) == L. But Wind's identity is the centered cursor, and the render
    // model's mapper already solves the whole centered geometry - including the edge zones, where
    // cursorScreen slides away from the center as the source rect clamps. So use the mapper's
    // CENTERED source rect for the transform and park the sprite at r.cursorScreen (screen px;
    // layered windows composite OUTSIDE the magnification, so screen coords are its native space):
    //   - content at the lens center (r.centerX/Y) displays at screen center; the sprite sits there;
    //   - the hidden REAL cursor is SetCursorPos'd to clickDesktop (= the lens center rounded), so a
    //     click lands exactly on the aimed content - same invariant as the render model;
    //   - at the edges the mapper moves cursorScreen off-center in lockstep with the clamped source,
    //     so sprite, content, and click point stay welded there too.
    // Bonus vs the old anchored design: the sprite now barely MOVES (center, except at edges), which
    // kills the sprite-lags-the-view wobble that plagued the anchored model during pans.
    // KEEP-ALIVE v2 (issue #148 action-start spike): DWM discards its magnification resources
    // when the transform VALUE sits still and pays a ~1fps rebuild on the next real change.
    // v1 jittered the LEVEL by an epsilon - that forced a full re-SCALE of every cached surface
    // every tick, whose cost grows with zoom (constant ~1fps at 16x: the cure was the disease).
    // v2 jitters only the private-channel TRANSLATION by 1 screen px on alternating ticks - a
    // re-COMPOSITE of already-scaled surfaces, cheap at any level - and only while within 1.5s
    // of the last REAL change: brief pauses (the pan-stop-pan pattern) stay hot, while true idle
    // lets DWM park legitimately (one spike after a long idle is acceptable; one per pause was not).
    // Ramp cost limiter (measured: 150-215ms spikes during zoom ramps at high level): every LEVEL
    // change makes DWM re-scale its cached surfaces, and the cost grows with the level. Apply the
    // ramping level at most every 3rd tick (48Hz on a 144Hz panel - visually still a smooth ramp);
    // panning updates stay per-tick at the applied level so the geometry is always consistent.
    // level==lastLevel_ (no ramp) and the 1x reset apply immediately.
    // Level applies STRAIGHT, per tick, continuously (probe-measured over Foundation: a per-tick
    // level ramp through the private channel costs the game ZERO >25ms frames at steady ~14ms
    // frametimes - identical class to the native Magnifier's eased notches). The earlier
    // quantization/divisor machinery created exactly the big discrete jumps that ARE expensive;
    // small continuous deltas are the cheap pattern. (Quantization removed after A/B.)
    // TDR guard (field: driver resets even at <=12x): above 8x, apply the ramping level only on
    // alternate ticks - halves the re-scale rate exactly where each re-scale is most expensive.
    // Held ticks recompute the source for the applied level so the geometry stays consistent.
    hiRampTick_ ^= 1;
    double applyLevel = level;
    if (lastLevel_ > 8.0 && level != lastLevel_ && level > 1.0 && hiRampTick_)
        applyLevel = lastLevel_;
    double srcL = r.srcLeft, srcT = r.srcTop;
    if (applyLevel != level) {
        OffsetF o = ComputeOffsetF(r.centerX, r.centerY, applyLevel, mon_.w, mon_.h);
        srcL = o.x; srcT = o.y;
    }
    const bool ramping = applyLevel != level || (applyLevel != lastLevel_ && lastLevel_ > 0.0);
    MagTransform m = ComputeMagTransform(srcL, srcT, applyLevel);
    const bool changed = m.offX != lastOffX_ || m.offY != lastOffY_ ||
                         m.txX != lastTxX_ || m.txY != lastTxY_ || applyLevel != lastLevel_;
    if (changed) {
        lastOffX_ = m.offX; lastOffY_ = m.offY; lastTxX_ = m.txX; lastTxY_ = m.txY;
        lastLevel_ = applyLevel;
        lastChangeMs_ = GetTickCount64();
        keepAliveTick_ = 0;
    }
    int txJitter = 0;
    // Keep-alive window shortened and disabled above 8x: high-level re-composites are the
    // expensive ones, and a parked pipeline is safer than a hot one next to a heavy game (TDR).
    if (!changed && !ramping && applyLevel <= 8.0 &&
        GetTickCount64() - lastChangeMs_ < 700) {
        keepAliveTick_ ^= 1;
        txJitter = keepAliveTick_;
    }
    host_.setTransform((float)applyLevel, m.offX, m.offY, m.txX + txJitter, m.txY, fastPan_);
    // Input transform (issue #148): teach the input stack the inverse mapping. Win32 mouse input
    // is proven correct without it (instrumented), but pointer-stack apps (Explorer XAML lists,
    // Chromium content) hit-test through this - without it they get level/edge-dependent dead
    // zones. Needs UIAccess; on the dev build the call fails and is logged once.
    // A/B knob (hot): magInputTransform=1 publishes the input transform; 0 (default) leaves the
    // OS default. Unvalidated for mouse (docs scope it to pen/touch) - measured live by the user.
    if (changed && cfg.magInputTransform != 0) {
        RECT src{ (LONG)(r.srcLeft + 0.5), (LONG)(r.srcTop + 0.5),
                  (LONG)(r.srcLeft + mon_.w / applyLevel + 0.5),
                  (LONG)(r.srcTop + mon_.h / applyLevel + 0.5) };
        RECT dst{ 0, 0, mon_.w, mon_.h };
        bool ok = host_.setInputTransform(applyLevel > 1.001, src, dst);
        if (!ok && !inputXformWarned_) {
            inputXformWarned_ = true;
            wind::Log(wind::LogLevel::Warn, "transform",
                      "MagSetInputTransform failed (no UIAccess?)");
        }
    } else if (changed && lastInputXformOn_) {
        RECT full{ 0, 0, mon_.w, mon_.h };
        host_.setInputTransform(false, full, full);
    }
    if (changed) lastInputXformOn_ = cfg.magInputTransform != 0;
    // FIELD-MEASURED (issue #148, this Windows build): DWM's fullscreen magnification DOES
    // magnify layered windows. So the sprite lives in DESKTOP coordinates at the lens center
    // (clickDesktop): the transform displays it AT the screen center (T(center) == cursorScreen,
    // including the edge zones where the mapper slides both), and it grows with zoom naturally,
    // exactly like the native Magnifier's pointer. No self-scaling (that double-scaled).
    (void)cfg;

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

    if (useSprite_ && sprite_ && ex.cursorLocked && ex.drawCursor) {
        // Inspect mode: the real cursor is frozen at the (overridden) click point, but the thing the
        // user aims with is the LOOK POINT (mapper center). Repaint the sprite as the crosshair (the
        // same design the render model draws) and put it on the look point, NOT on cx/cy - those are
        // pinned to the frozen cursor while Inspect is on. The transform is anchored at the look
        // point (T(L) == L) and this layered window composites unmagnified, so the crosshair sits
        // exactly on the aimed content at any zoom, including the 1x roam. Without this branch the
        // sprite kept drawing the arrow at the frozen point (visible, stationary) and no crosshair
        // existed at all - the transform model used to ignore ex.cursorLocked.
        sprite_->showCrosshair();
        sprite_->moveTo(r.clickDesktopX + mon_.x, r.clickDesktopY + mon_.y);
        sprite_->keepOnTop();
    } else if (useSprite_ && sprite_ && ex.drawCursor) {
        CursorSprite::ShapeStatus st = sprite_->refreshShape();
        if (st == CursorSprite::ShapeStatus::Rendered) {
            // Desktop coords at the lens center: the (magnifying) transform displays the sprite
            // at the screen center - the exact spot where the aimed content and the click land.
            sprite_->moveTo(cx, cy);
            sprite_->show();
            // Composited outside the magnification, so it must fight for real z-order: reclaim the top
            // of our band when a popup (tray/context menu, flyout) has been raised over us. Throttled.
            sprite_->keepOnTop();
        } else {
            sprite_->hide();   // Hidden/Unsupported: show the real (or app-custom) cursor instead
        }
    } else if (useSprite_ && sprite_) {
        // cursorVisibility=never, or the hide-cursor hotkey. The block above is what MOVES the sprite,
        // so skipping it is not enough: hideSystemCursor(true) already showed the sprite at activation
        // and it would freeze on screen, visible and no longer tracking. Hide it explicitly. hide() is
        // idempotent, and the real cursor stays blanked (CursorBlanker is independent of this flag),
        // so nothing unmagnified reappears; zoom-out restores it via hideSystemCursor(false).
        sprite_->hide();
    }

    if (smoothPan_ && level > 1.0) {
        unsigned long long now = GetTickCount64();
        if (now - lastPinAssertMs_ >= 500) { lastPinAssertMs_ = now; pin_.assert_(); }
    } else {
        pin_.hide();
    }
}

void TransformModel::shutdown() {
    if (sprite_) sprite_->destroy();
    if (blanker_) blanker_->restore();
    pin_.destroy();
    host_.shutdown();
    ready_ = false;
}
}
