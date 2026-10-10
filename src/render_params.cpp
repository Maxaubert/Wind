// Pure render-parameter fill (no engine, no Win32): split out of render_model.cpp so the test build
// can compile it (tests/test_render_params.cpp).
#include "render_model.h"

namespace wind {

int CursorModeFromCfg(const Config& c) {
    if (c.cursorVisibility == "never")  return 2;
    if (c.cursorVisibility == "always") return 1;
    return 0;
}

void FillRenderParams(RenderFrameParams& p, const MapResult& r, const Config& cfg,
                      const MonitorTarget& mon, double level) {
    p.level = level;
    p.srcLeft = r.srcLeft; p.srcTop = r.srcTop;
    p.cursorScreenX = r.cursorScreenX; p.cursorScreenY = r.cursorScreenY;
    // clickDesktop is local monitor px; SetCursorPos needs virtual-desktop coords.
    p.clickDesktopX = r.clickDesktopX + mon.x; p.clickDesktopY = r.clickDesktopY + mon.y;
    p.cursorScaleWithZoom = (cfg.cursorConstantSize == 0);   // issue #253
    p.bilinear = (cfg.bilinear != 0);
    p.sharpness = cfg.sharpness;
    p.brightness = cfg.brightness;
    p.cursorMode = CursorModeFromCfg(cfg);
    // In DwmFlush mode we present immediately (no vsync block) and let DwmFlush() pace.
    p.vsync = (cfg.vsync != 0 && cfg.dwmFlush == 0);
    p.cropCapture = (cfg.cropCapture != 0);
    p.outline = OutlineVisibleAtLevel(cfg, level);
    p.outlineThicknessPx = cfg.outlineThickness;
    p.outlineR = cfg.outlineR; p.outlineG = cfg.outlineG; p.outlineB = cfg.outlineB;   // parsed once in ParseConfig
    p.outlineAlpha = 1.0f;   // RunTick lowers this when idle-hide is active
    p.cursorLocked = false;  // RunTick sets true while zoomed + Inspect mode (draw the crosshair sprite)
    p.suppressCursorSync = false;  // RunTick sets true mid-drag (issue #169; see PresentExtras)
}
}
