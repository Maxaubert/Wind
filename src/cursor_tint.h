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
    // Puts the pristine pointers back with direct SetSystemCursor calls (no scheme reload).
    void restore();
    // Someone else (an engine's scheme reload) already put the scheme back: forget our state so the
    // next apply() re-tints.
    void invalidate() { applied_ = false; }
    bool applied() const { return applied_; }

private:
    static constexpr int kCount = 14;
    HCURSOR pristine_[kCount] = {};
    bool mono_[kCount] = {};
    bool applied_ = false;
    ColorMatrix current_{};
    void release();
};

}  // namespace wind
