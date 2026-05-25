#pragma once
namespace wind {
// One frame's mapping result for the own renderer (centered cursor mode).
struct MapResult {
    double srcLeft, srcTop;              // float top-left of the source region (desktop px)
    double cursorScreenX, cursorScreenY; // where to draw the cursor sprite (screen px)
    int    clickDesktopX, clickDesktopY; // where to SetCursorPos for click hit-testing
};
// Pure centered-mode mapper. Integrates raw-input deltas into a float lens center
// (desktop px), so the world pans with sub-pixel precision while the cursor stays at
// screen center - shifting toward an edge only when the view clamps at the desktop edge.
// The click point equals the lens center: the desktop point under the drawn cursor, so a
// click through the transparent overlay lands exactly there.
class CursorMapper {
public:
    CursorMapper(int screenW, int screenH, double sensitivity);
    void reset(double centerX, double centerY);    // pin lens center (e.g. on zoom-in)
    MapResult update(int rawDx, int rawDy, double level);
    double centerX() const { return cx_; }
    double centerY() const { return cy_; }
private:
    int sw_, sh_;
    double sens_;
    double cx_, cy_;
};
}
