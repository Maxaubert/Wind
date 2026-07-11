#include "transform_model.h"
#include "transform.h"   // ComputeMagTransform
#include "logging.h"
#include <windows.h>
#include <magnification.h>   // MagShowSystemCursor (global cursor hide) + TEMP MagGetFullscreenTransform diag

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
    // Two layers, both needed. The blanker blanks the STANDARD cursor scheme (so the sprite can
    // render those shapes from its pre-blank originals), but apps that set privately-loaded cursor
    // handles (Explorer's hand, WinUI resize shapes) are outside any scheme and stayed visible.
    // MagShowSystemCursor hides the real cursor GLOBALLY, any handle, any shape - the same call the
    // render model relies on. Restored on every teardown path (hide(false), shutdown, and main's
    // crash/atexit/eviction nets, which already call MagShowSystemCursor(TRUE) + SPI_SETCURSORS).
    if (hide) { blanker_->blank(); MagShowSystemCursor(FALSE); if (sprite_) sprite_->show(); }
    else      { if (sprite_) sprite_->hide(); MagShowSystemCursor(TRUE); blanker_->restore(); }
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
    const bool inspect = ex.cursorLocked;
    // Refresh the sprite's shape for this tick (repaints the layered window only when the shape
    // actually changed). During Inspect the sprite is our crosshair, so this is skipped there.
    CursorSprite::ShapeStatus st = CursorSprite::ShapeStatus::Hidden;
    if (useSprite_ && sprite_ && ex.drawCursor && !inspect) st = sprite_->refreshShape();

    // CENTERED vs ANCHORED (spec: docs/superpowers/specs/2026-07-11-transform-centered-cursor-design.md).
    // DWM composites the cursor AND layered windows OUTSIDE the fullscreen magnification (measured:
    // a layered window at desktop P draws at screen P, unscaled), so the two geometries are:
    //  - CENTERED (the render model's): source rect = the mapper's centered clamped rect; the sprite
    //    is drawn at cursorScreen = T(C) (screen center in steady state, edge-shifted at clamps);
    //    the OS cursor is welded to the lens center C, so a click lands exactly on the content under
    //    the sprite. The real cursor is hidden GLOBALLY (MagShowSystemCursor + the blanker) and the
    //    sprite renders ANY shape (standard from the pre-blank originals, private handles live), so
    //    the geometry is stable for the whole session - it must NEVER flip on a cursor-shape change
    //    (that lurches the view and staled hover state; found live in issue #139 testing).
    //  - ANCHORED: off = L*(1 - 1/level) makes T(L) == L, so a real cursor drawn by DWM at its own
    //    desktop spot sits on exactly what it clicks. The only correct geometry when we do not draw
    //    the cursor ourselves: cursorSprite=0, or transformCenterCursor=0.
    const bool centered = cfg.transformCenterCursor != 0 && useSprite_ && sprite_;

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

    // TEMP DIAGNOSTIC (issue #139, centered-mode mixed-geometry report): once per second while
    // active, log what we SENT vs what the OS says is APPLIED (MagGetFullscreenTransform read-back)
    // and where the OS cursor actually sits after the weld. Removed once the report is explained.
    unsigned long long nowDiag = GetTickCount64();
    if (nowDiag - lastDiagMs_ >= 1000) {
        lastDiagMs_ = nowDiag;
        float gl = 0.0f; int gx = 0, gy = 0;
        BOOL got = MagGetFullscreenTransform(&gl, &gx, &gy);
        POINT ap{}; GetCursorPos(&ap);
        Log(LogLevel::Info, "cgeo",
            "L=%.2f centered=%d priv=%d src=(%.1f,%.1f) sent off=(%d,%d) tx=(%d,%d) "
            "readback ok=%d L=%.2f off=(%d,%d) weld=(%d,%d) actual=(%ld,%ld) curScr=(%.1f,%.1f) click=(%d,%d)",
            level, (int)centered, (int)host_.privateActive(), src.x, src.y, m.offX, m.offY,
            m.txX, m.txY, (int)got, gl, gx, gy, cx, cy, ap.x, ap.y,
            r.cursorScreenX, r.cursorScreenY, r.clickDesktopX, r.clickDesktopY);
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
            sprite_->hide();   // Hidden: suppressed/hidden cursor or capture failure - show nothing
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
    if (useSprite_) MagShowSystemCursor(TRUE);   // never leave the OS cursor hidden (CLAUDE.md rule)
    if (blanker_) blanker_->restore();
    pin_.destroy();
    host_.shutdown();
    ready_ = false;
}
}
