#include "transform_model.h"
#include "transform.h"   // ComputeMagTransform
#include <windows.h>

namespace wind {

bool TransformModel::initialize(const MonitorTarget& monitor) {
    mon_ = monitor;
    if (!host_.initialize()) return false;
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
    if (hide) { blanker_->blank(); if (sprite_) sprite_->show(); }
    else      { if (sprite_) sprite_->hide(); blanker_->restore(); }
}

void TransformModel::setActive(bool active) {
    active_ = active;
    if (!active) {
        host_.setTransform(1.0f, 0, 0, 0, 0, false);   // back to 1x
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
    MagTransform m = ComputeMagTransform(r.srcLeft, r.srcTop, level);
    host_.setTransform((float)level, m.offX, m.offY, m.txX, m.txY, fastPan_);
    // The sprite composites unmagnified, so matching the zoom is our job (integer scale, capped).
    if (useSprite_ && sprite_) {
        int scale = cfg.cursorScaleWithZoom ? (int)(level + 0.5) : 1;
        sprite_->setScale(scale);
    }

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
        sprite_->moveTo((int)(r.cursorScreenX + 0.5) + mon_.x, (int)(r.cursorScreenY + 0.5) + mon_.y);
        sprite_->keepOnTop();
    } else if (useSprite_ && sprite_ && ex.drawCursor) {
        CursorSprite::ShapeStatus st = sprite_->refreshShape();
        if (st == CursorSprite::ShapeStatus::Rendered) {
            // Park the sprite at the mapper's cursorScreen: the screen center normally, sliding
            // off-center at the edges in lockstep with the clamped source rect - the exact spot
            // where the aimed content (and the hidden real cursor's click) lands.
            sprite_->moveTo((int)(r.cursorScreenX + 0.5) + mon_.x, (int)(r.cursorScreenY + 0.5) + mon_.y);
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
