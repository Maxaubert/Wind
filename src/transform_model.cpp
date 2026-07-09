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
    // The real cursor is composited OUTSIDE the magnification, so it is drawn at the unmagnified
    // click point while its target is drawn at T(click). Aiming at it is the click drift. Suppress
    // every shape: MagShowSystemCursor covers app-custom cursors that SetSystemCursor cannot touch.
    // Verified safe: it leaves CURSORINFO's SHOWING/SUPPRESSED flags and hCursor untouched, so the
    // sprite can still read the shape it must draw. Do it even with the sprite off, so nothing leaks.
    host_.showSystemCursor(!hide);
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
        haveOff_ = false;         // re-anchor the pan offset at the cursor on the next zoom-in
    }
}

// One axis of the edge-triggered pan. `c` is the cursor in desktop px, `span` the screen size on that
// axis. The cursor is drawn at screen s = (c - off) * level. Hold `off` still while s stays inside the
// margins; when s reaches one, move `off` just enough to pin s to that margin. The offset is clamped
// to the legal source range, so at the desktop edges the cursor keeps travelling into the corner
// rather than the view trying to pan past the desktop.
static double PanAxis(double off, double c, double level, double span, double margin) {
    if (level <= 1.0) return 0.0;                       // 1x: the whole desktop is the view
    const double maxOff = span - span / level;          // >= 0
    double s = (c - off) * level;
    if (s < margin)          off = c - margin / level;
    else if (s > span - margin) off = c - (span - margin) / level;
    if (off < 0.0) off = 0.0; else if (off > maxOff) off = maxOff;
    return off;
}

// Seed on the first active tick, and re-anchor whenever the zoom level changes, so that the content
// under the cursor stays exactly where it is while zooming (the cursor is the zoom's fixed point).
// Without this the world would slide under the pointer during the zoom ramp.
void TransformModel::UpdatePanOffset(double cx, double cy, double level) {
    const double sw = (double)mon_.w, sh = (double)mon_.h;
    // Margin: the cursor may roam freely inside this inset before the view starts to pan.
    const double marginX = sw * 0.10, marginY = sh * 0.10;

    if (!haveOff_) {
        // Anchor at the cursor: T(c) == c, so zooming in does not move what is under the pointer.
        offX_ = cx * (1.0 - 1.0 / (level > 1.0 ? level : 1.0));
        offY_ = cy * (1.0 - 1.0 / (level > 1.0 ? level : 1.0));
        lastLevel_ = level;
        haveOff_ = true;
    } else if (level != lastLevel_) {
        // Keep the cursor's screen point fixed across the level change: (c-off)*lvl is invariant.
        const double sx = (cx - offX_) * lastLevel_;
        const double sy = (cy - offY_) * lastLevel_;
        if (level > 1.0) { offX_ = cx - sx / level; offY_ = cy - sy / level; }
        else             { offX_ = 0.0; offY_ = 0.0; }
        lastLevel_ = level;
    }

    offX_ = PanAxis(offX_, cx, level, sw, marginX);
    offY_ = PanAxis(offY_, cy, level, sh, marginY);
}

void TransformModel::present(const MapResult& r, double level, const Config& cfg,
                             const MonitorTarget& mon, const PresentExtras& ex) {
    (void)cfg; (void)mon;
    // Edge-triggered pan. The sprite is an UpdateLayeredWindow window, which DWM magnifies along with
    // the content (measured), so a sprite at desktop C is drawn at T(C) - exactly where the item a
    // click at C selects is drawn. Clicks are therefore accurate for ANY offset, which frees the pan
    // policy entirely. The real cursor is NOT magnified, so it must stay suppressed (hideSystemCursor)
    // or it would appear at C while its target is at T(C).
    //
    // So: hold the offset still while the cursor roams, and pan only when it reaches a margin at the
    // screen edge. Re-anchor on every zoom change so the point under the cursor does not slide.
    UpdatePanOffset(r.centerX, r.centerY, level);
    MagTransform m = ComputeMagTransform(offX_, offY_, level);
    host_.setTransform((float)level, m.offX, m.offY, m.txX, m.txY, fastPan_);

    // Weld the hidden OS cursor to the lens point, exactly as RenderEngine::render does. This keeps
    // the scene-locked sprite on the real click point AND keeps RunTick's warp-and-measure pan
    // tracking consistent (RunTick assumes the cursor was moved here each active tick). Deduped so an
    // idle tick injects no synthetic mouse move. Inspect freeze pins the point via ex.clickOverride;
    // otherwise clickDesktop is monitor-local, so add the monitor origin for desktop px.
    int cx = ex.clickOverride ? ex.clickDesktopX : (r.clickDesktopX + mon_.x);
    int cy = ex.clickOverride ? ex.clickDesktopY : (r.clickDesktopY + mon_.y);
    // Pin the hidden OS cursor to the lens point on EVERY active tick, not just when the lens point
    // changes. The pan delta is divided by the zoom level, so at 4x the lens centre advances a quarter
    // as fast as the hand - and `round(C)` can sit still for several ticks while the physical mouse
    // keeps dragging the OS cursor away at full speed. Re-pinning only on change therefore let the
    // real cursor wander off the lens point, so hover and clicks landed wherever it had drifted to,
    // in bands that grow with zoom. Dragging still worked because the app had captured the mouse.
    // Compare against the live position rather than a cached one, so an idle tick still injects no
    // synthetic mouse move, and any external move (another app calling SetCursorPos) is corrected.
    POINT osCur{};
    if (!GetCursorPos(&osCur) || osCur.x != cx || osCur.y != cy) SetCursorPos(cx, cy);

    if (useSprite_ && sprite_ && ex.drawCursor) {
        CursorSprite::ShapeStatus st = sprite_->refreshShape();
        if (st == CursorSprite::ShapeStatus::Rendered) {
            // Draw at the click point. DWM does NOT magnify this layered window (measured), so it is
            // drawn at screen (cx, cy) unscaled - and because the transform is anchored at the cursor
            // (T(cx,cy) == (cx,cy)), the content there is exactly the content a click at (cx, cy)
            // hits. The sprite therefore sits on its own target at any zoom, anywhere on screen.
            sprite_->moveTo(cx, cy);
            sprite_->show();
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
