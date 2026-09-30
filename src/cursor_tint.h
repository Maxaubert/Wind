#pragma once
// Tinted system pointers at 1x (spec 2026-09-30-cursor-tint-design.md). Windows draws the pointer on
// a hardware plane the DWM colour effect never reaches, so while Warmth/Brightness are on Wind swaps
// the standard pointers for tinted copies. Tick thread only.
#include <windows.h>
#include "color_matrix.h"

namespace wind {

class CursorTint {
public:
    ~CursorTint();
    // Pristine copies of the standard pointers. Call when the scheme is known clean: at start-up
    // after the scheme reload, and on WM_SETTINGCHANGE/SPI_SETCURSORS (the user changed it).
    void capture();
    // Tints every non-animated standard pointer with `m` (the sRGB-encoded matrix). No-op when the
    // same matrix is already applied. The identity restores instead.
    void apply(const ColorMatrix& m);
    // Puts the pointers back. reloadScheme=true (idle: colour off, fullscreen app, exit) reloads the
    // user's real scheme, which keeps multi-size, DPI-scaling pointers. false (zoom-in) swaps the
    // in-memory copies directly: no registry read, no broadcast, safe on the zoom-in path, and only
    // momentary because zoom-out reloads the scheme anyway. Measured 2026-09-30: copies differ
    // slightly from the scheme's own pointers (fixed size, the text beam's format), so they must
    // never be what stays on screen.
    void restore(bool reloadScheme);
    // Someone else (an engine's scheme reload) already put the scheme back: forget our state so the
    // next apply() re-tints.
    void invalidate() { applied_ = false; }
    bool applied() const { return applied_; }

private:
    static constexpr int kCount = 14;
    HCURSOR pristine_[kCount] = {};
    bool mono_[kCount] = {};
    bool applied_ = false;
    // The system pointers are ours (tinted, or the in-memory copies from a zoom-in swap) rather than
    // the user's scheme. Separate from applied_: a zoom-in swap and invalidate() clear applied_, but
    // the render engine never reloads the scheme at zoom-out, so the copies can still be on screen
    // (review 2026-09-30). Only a real scheme reload clears it.
    bool swapped_ = false;
    ColorMatrix current_{};
    void release();
};

}  // namespace wind
