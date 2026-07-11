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
    // Resolve the sprite verdict FIRST: it decides which offset this tick uses. During Inspect the
    // sprite is our crosshair (always placeable), so refreshShape is only consulted for the normal
    // cursor path.
    const bool inspect = ex.cursorLocked;
    CursorSprite::ShapeStatus st = CursorSprite::ShapeStatus::Hidden;
    if (useSprite_ && sprite_ && ex.drawCursor && !inspect) st = sprite_->refreshShape();

    // CENTERED vs ANCHORED (spec: docs/superpowers/specs/2026-07-11-transform-centered-cursor-design.md).
    // DWM composites the cursor AND layered windows OUTSIDE the fullscreen magnification (measured:
    // a layered window at desktop P draws at screen P, unscaled), so the two geometries are:
    //  - CENTERED (the render model's): source rect = the mapper's centered clamped rect; the sprite
    //    is drawn at cursorScreen = T(C) (screen center in steady state, edge-shifted at clamps);
    //    the OS cursor is welded to the lens center C, so a click lands exactly on the content under
    //    the sprite. Requires that the visible cursor is one WE place (the sprite or the Inspect
    //    crosshair), or that no cursor is visible at all (drawCursor off / shape Hidden - the real
    //    cursor is blanked or app-suppressed).
    //  - ANCHORED: off = L*(1 - 1/level) makes T(L) == L, so a real cursor drawn by DWM at its own
    //    desktop spot sits on exactly what it clicks. Used when an app-custom shape (Unsupported)
    //    cannot be blanked, when cursorSprite=0, and when transformCenterCursor=0.
    // A standard<->custom shape transition steps the view once; rare and correct.
    const bool centered = cfg.transformCenterCursor != 0 && useSprite_ && sprite_
                       && (inspect || !ex.drawCursor || st != CursorSprite::ShapeStatus::Unsupported);

    OffsetF src = centered ? OffsetF{ r.srcLeft, r.srcTop }
                           : ComputeFixedPointOffset(r.centerX, r.centerY, level);
    MagTransform m = ComputeMagTransform(src.x, src.y, level);
    host_.setTransform((float)level, m.offX, m.offY, m.txX, m.txY, fastPan_);

    // Weld the hidden OS cursor to the lens point, exactly as RenderEngine::render does. This keeps
    // clicks landing at the lens center AND keeps RunTick's warp-and-measure pan tracking consistent
    // (RunTick assumes the cursor was moved here each active tick). Deduped so an idle tick injects
    // no synthetic mouse move. Inspect freeze pins the point via ex.clickOverride; otherwise
    // clickDesktop is monitor-local, so add the monitor origin for desktop px.
    int cx = ex.clickOverride ? ex.clickDesktopX : (r.clickDesktopX + mon_.x);
    int cy = ex.clickOverride ? ex.clickDesktopY : (r.clickDesktopY + mon_.y);
    if (!haveLastClick_ || cx != lastClickX_ || cy != lastClickY_) {
        SetCursorPos(cx, cy);
        lastClickX_ = cx; lastClickY_ = cy; haveLastClick_ = true;
    }

    // Where the drawn cursor goes on SCREEN. Centered: cursorScreen (= T(C), the screen point that
    // shows the lens-center content - the sprite sits on exactly what a click at C hits). Anchored:
    // the click point itself (T(L) == L there). The layered sprite composites unmagnified, so its
    // window position IS its screen position.
    const int sx = centered ? (int)(r.cursorScreenX + 0.5) + mon_.x : cx;
    const int sy = centered ? (int)(r.cursorScreenY + 0.5) + mon_.y : cy;

    if (useSprite_ && sprite_ && inspect && ex.drawCursor) {
        // Inspect mode: the real cursor is frozen at the (overridden) click point, but the thing the
        // user aims with is the LOOK POINT (mapper center). Repaint the sprite as the crosshair (the
        // same design the render model draws) and put it on the look point's SCREEN position:
        // cursorScreen under the centered rect; with the anchored offset T(L) == L makes that the
        // look point's desktop position itself. NOT cx/cy - those are pinned to the frozen cursor.
        sprite_->showCrosshair();
        sprite_->moveTo(centered ? sx : r.clickDesktopX + mon_.x,
                        centered ? sy : r.clickDesktopY + mon_.y);
        sprite_->keepOnTop();
    } else if (useSprite_ && sprite_ && ex.drawCursor) {
        if (st == CursorSprite::ShapeStatus::Rendered) {
            sprite_->moveTo(sx, sy);
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
