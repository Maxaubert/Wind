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
    // Call between BeginDraw and EndDraw. Paints the whole flyout from the origin in DIPs onto an
    // opaque target; ApplyShapeAlpha then cuts the rounded corners out of the pixels.
    void Draw(const View& v, const Geometry& g);
    // The profile list popup (its own window, its own painter): same tokens, same clipping.
    void DrawList(const ListView& v, const ListGeometry& g);
private:
    struct Impl;
    Impl* d_;
};

// The painter draws onto an OPAQUE target (so ClearType text works); this cuts the rounded shape
// out of the finished pixels: alpha 255 inside, an antialiased 0 outside the kRadius corners.
// `premultiply` for UpdateLayeredWindow, false for a straight-alpha PNG. dpi = the target's DPI.
void ApplyShapeAlpha(unsigned char* bgra, int w, int h, int strideBytes, int dpi, bool premultiply);

// Renders the flyout to a PNG at `dpi` (96 = 1x). False on any failure.
bool RenderToPng(const View& v, int profileTextW, int dpi, const wchar_t* path);
// The same for the list popup (profiles, or the engine list with its caption).
bool RenderListToPng(const ListView& v, const ListGeometry& g, int dpi, const wchar_t* path);

}}  // namespace wind::Flyout
