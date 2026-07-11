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

    // The lens center C in virtual-desktop px: the point input should ACT at (clicks, hover).
    int cx = r.clickDesktopX + mon_.x;
    int cy = r.clickDesktopY + mon_.y;

    // Sprite placement depends on the COMPOSITING LAW for the sprite's window kind (cursor_sprite.h
    // inBand()): BANDED windows are re-magnified with the desktop, so the sprite is placed at the
    // DESKTOP point it represents and the transform carries its image to T(point) - on the aimed
    // content at every zoom, every position, clamps included (and scales it with zoom, like the
    // render model's default). This invariant is unobservable under ANCHORED geometry (T(L) == L
    // makes the placement a fixed point either way), which is why the banded re-magnification went
    // undetected until centered mode separated the two laws (issue #139 marker test). PLAIN layered
    // windows composite unmagnified at raw coords (PR #130 marker measurement), so they are placed
    // at the intended SCREEN point directly.
    const bool bandComposited = sprite_ && sprite_->inBand();
    const int sx = bandComposited ? cx : (centered ? (int)(r.cursorScreenX + 0.5) + mon_.x : cx);
    const int sy = bandComposited ? cy : (centered ? (int)(r.cursorScreenY + 0.5) + mon_.y : cy);

    // CENTERED mode is FREE-FOLLOW: the real cursor is NEVER moved by us (no weld, no synthetic
    // input). RunTick snaps the lens center to the real cursor each tick, so C == the raw cursor
    // and input acts natively on exactly the content shown under the sprite (input acts at the
    // RAW cursor - measured: GetCursorPos == GetPhysicalCursorPos always, no OS input
    // virtualization under the fullscreen transform). Do NOT reintroduce any SetCursorPos here in
    // centered mode: a welding stream fights the hand's motion and intermittently breaks app hover
    // tracking (issue #139 LEDGER, builds B1-B5). The weld remains only where WE own the cursor
    // position: ANCHORED mode (realizes cursorSensitivity/smoothing, the old shipped behavior) and
    // the Inspect freeze override.
    const int wx = ex.clickOverride ? ex.clickDesktopX : cx;
    const int wy = ex.clickOverride ? ex.clickDesktopY : cy;
    if (!centered || ex.clickOverride) {
        POINT rawNow{};
        GetCursorPos(&rawNow);
        if (rawNow.x != wx || rawNow.y != wy) SetCursorPos(wx, wy);
    }
    lastWeldX_ = wx; lastWeldY_ = wy;

    // TEMP DIAGNOSTIC (issue #139): once per second while active, log sent vs applied transform
    // (MagGetFullscreenTransform read-back), the weld, and T^-1(weld) - which must equal the lens
    // center C for input to act under the drawn cursor. Removed once verified live.
    unsigned long long nowDiag = GetTickCount64();
    if (nowDiag - lastDiagMs_ >= 1000) {
        lastDiagMs_ = nowDiag;
        float gl = 0.0f; int gx = 0, gy = 0;
        BOOL got = MagGetFullscreenTransform(&gl, &gx, &gy);
        POINT ap{}; GetCursorPos(&ap);
        POINT pp{}; BOOL gotPhys = GetPhysicalCursorPos(&pp);
        // What actually sits under the weld point (hover goes to this window), and whether a
        // foreign/stale ClipCursor is clamping the weld - the two untested candidates for the
        // "hover stops updating after panning a while" report (see the LEDGER doc).
        HWND under = WindowFromPoint(ap);
        wchar_t clsW[64] = L""; char cls[64] = "";
        if (under) {
            GetClassNameW(under, clsW, 64);
            WideCharToMultiByte(CP_UTF8, 0, clsW, -1, cls, sizeof(cls), nullptr, nullptr);
        }
        RECT clip{}; GetClipCursor(&clip);
        Log(LogLevel::Info, "cgeo",
            "L=%.2f centered=%d src=(%.1f,%.1f) readback ok=%d off=(%d,%d) weld=(%d,%d) "
            "logical=(%ld,%ld) phys=(%d:%ld,%ld) C=(%d,%d) curScr=(%.1f,%.1f) "
            "under=%p cls=%s clip=(%ld,%ld,%ld,%ld)",
            level, (int)centered, src.x, src.y, (int)got, gx, gy, wx, wy,
            ap.x, ap.y, (int)gotPhys, pp.x, pp.y, cx, cy, r.cursorScreenX, r.cursorScreenY,
            (void*)under, cls, clip.left, clip.top, clip.right, clip.bottom);
    }

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
