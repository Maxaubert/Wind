#pragma once
// The tray flyout's drawing half (Direct2D + DirectWrite + WIC). It paints a View (flyout_model.h)
// onto ANY ID2D1RenderTarget in DIPs, so the live window (a DC target pushed through
// UpdateLayeredWindow for true per-pixel alpha and exact rounded corners) and `--render-test`
// (a WIC bitmap saved as PNG) run the very same code. Tokens come from
// docs/design/tray-2026-10/j01-calm-tint.html and docs/design/settings-2026-10/FINAL-v10-grey.html.
#include <windows.h>
#include <d2d1.h>
#include <string>
#include "flyout_model.h"

namespace wind { namespace Flyout {

// Shared factories, text formats and the decoded aurora image. Main thread only; COM must be
// initialised. Safe to call twice.
bool DrawInit();
void DrawShutdown();

// Width in DIPs of the profile name in the bottom row (the layout needs it for the button).
int MeasureProfileText(const std::wstring& name);

// A render target for the live window: BindDC it to a 32-bit premultiplied DIB each frame.
// The caller owns (Release) the result. dpi = the monitor's effective DPI.
ID2D1DCRenderTarget* CreateDcTarget(int dpi);

// One painter per render target (it caches brushes and the aurora bitmap on it).
class Painter {
public:
    Painter();
    ~Painter();
    bool Init(ID2D1RenderTarget* rt, bool dark);
    // Call between BeginDraw and EndDraw. Paints the whole flyout (transparent outside the
    // rounded shape) from the origin in DIPs.
    void Draw(const View& v, const Geometry& g);
private:
    struct Impl;
    Impl* d_;
};

// Renders the flyout to a PNG at `dpi` (96 = 1x). False on any failure.
bool RenderToPng(const View& v, int profileTextW, int dpi, const wchar_t* path);

}}  // namespace wind::Flyout
