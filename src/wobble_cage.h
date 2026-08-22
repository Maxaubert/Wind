#pragma once
#include <windows.h>

namespace wind {

// WOBBLE CAGE (issue #229): a live, visible wobble detector - four 10px bars boxing the cursor,
// and the bar that gets hit flashes red.
//
// WHY THE COLLISION IS NUMERIC, NOT OPTICAL. Everything Wind can draw is a layered window, and
// DWM magnifies those along with the content (field-measured; the band-16 experiment did not
// escape it either). So a cage pinned to screen space cannot exist while the transform is live:
// it would be displaced by exactly the same amount as the thing it is supposed to catch, and
// the two would never collide. The cage is therefore drawn around the cursor in DESKTOP space -
// where it stays framed around the pointer - while the hit test compares the sprite's screen
// position under the LIVE transform against the screen centre, which is where a centred view
// must put it. Same arithmetic as the proving ground's sprOff metric (0.6px on a good build,
// 27.4px on a wobbling one), rendered instead of logged.
//
// Bars are sized to read ~10px on screen: thickness = max(1, 10/level) desktop px, since a
// magnified window cannot draw a sub-pixel feature at high zoom.
class WobbleCage {
public:
    ~WobbleCage() { destroy(); }
    bool create(int zorderBand);
    // Box half-extent in DESKTOP px (the bars magnify with the cursor, so this stays constant
    // in desktop space and the frame keeps its proportion at every zoom). `body` re-centres the
    // box from the cursor's hotspot onto its drawn body.
    void setSize(int half, int body) { half_ = half > 4 ? half : 4; body_ = body; }
    void destroy();
    // Places the cage around `desktopX/Y` for `level`, and lights the bar(s) the sprite crossed:
    // offX/offY are the sprite's screen displacement from centre (px). Anything beyond
    // `gapScreenPx` counts as a hit and flashes that side for ~250ms.
    // `desktopX/Y` is the cursor HOTSPOT; the cage centres on the cursor BODY (hotspot plus
    // half a cursor) so the bars sit flush around what you actually see, not around the arrow
    // tip. `clampedX/Y` suppress an axis whose view is clamped at a screen edge - there the
    // sprite leaves the centre legitimately and a hit test would cry wolf all day.
    void update(int desktopX, int desktopY, double level, double offX, double offY,
                double gapScreenPx, bool clampedX, bool clampedY);
    void hide();
    // Hits since creation, per side (left, right, top, bottom) - the numeric half of the tool.
    void hits(unsigned& left, unsigned& right, unsigned& top, unsigned& bottom) const;

private:
    void redraw(double level);
    int  half_ = 18, body_ = 9;
    HWND hwnd_ = nullptr;
    bool visible_ = false;
    int  lastX_ = INT_MIN, lastY_ = INT_MIN, lastSide_ = 0;
    double lastLevel_ = 0.0, lastGap_ = 0.0;
    unsigned long long flashUntil_[4] = { 0, 0, 0, 0 };   // L, R, T, B
    unsigned hits_[4] = { 0, 0, 0, 0 };
};

}
