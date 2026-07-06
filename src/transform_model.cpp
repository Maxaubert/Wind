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
        sprite_->create();
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
    }
}

void TransformModel::present(const MapResult& r, double level, const Config& cfg,
                             const MonitorTarget& mon, const PresentExtras& ex) {
    (void)cfg; (void)mon;
    MagTransform m = ComputeMagTransform(r.srcLeft, r.srcTop, level);
    host_.setTransform((float)level, m.offX, m.offY, m.txX, m.txY, fastPan_);

    if (useSprite_ && sprite_ && ex.drawCursor) {
        CursorSprite::ShapeStatus st = sprite_->refreshShape();
        if (st == CursorSprite::ShapeStatus::Rendered) {
            // The sprite lives in unmagnified desktop coords at the cursor's click point (the same
            // point the transform maps to screen center-ish), so the transform magnifies it welded
            // to the content. clickDesktop is monitor-local; add the monitor origin for desktop px.
            sprite_->moveTo(r.clickDesktopX + mon_.x, r.clickDesktopY + mon_.y);
            sprite_->show();
            if (sprite_->needsPolarity()) sprite_->setPolarity(true);   // dark ink; polarity sampling TBD
        } else {
            sprite_->hide();   // Hidden/Unsupported: show the real (or app-custom) cursor instead
        }
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
