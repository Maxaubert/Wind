#pragma once
#include "render_engine.h"   // MonitorTarget
#include "cursor_mapper.h"    // MapResult
#include "config.h"           // Config
namespace wind {
// Per-tick render-only overrides RunTick computes (outline fade, inspect crosshair, click freeze,
// cursor hide). The transform model ignores everything except drawCursor.
struct PresentExtras {
    bool  outline = false;        // draw the edge outline this frame
    float outlineAlpha = 1.0f;    // idle-fade alpha
    bool  cursorLocked = false;   // Inspect mode: draw the crosshair at the look point
    int   cursorMode = 0;         // 0=auto,1=always,2=never (final, after cursorHidden override)
    int   clickDesktopX = 0;      // SetCursorPos target override (Inspect freeze); <INT_MIN if unset
    int   clickDesktopY = 0;
    bool  clickOverride = false;  // true when clickDesktop* should replace the mapper's click point
    bool  drawCursor = true;      // whether a cursor should be shown at all this frame
};
struct IMagnifierModel {
    virtual ~IMagnifierModel() = default;
    virtual bool initialize(const MonitorTarget& monitor) = 0;
    virtual void shutdown() = 0;
    virtual bool ready() const = 0;
    virtual void hideSystemCursor(bool hide) = 0;
    virtual void setActive(bool active) = 0;          // reveal/hide overlay, or enable/disable transform
    virtual void onActivate() {}                      // called on idle->active (render: invalidateCapture/prime)
    virtual bool retarget(const MonitorTarget& m) { (void)m; return false; }  // render-only; false = unchanged
    virtual void present(const MapResult& r, double level, const Config& cfg,
                         const MonitorTarget& mon, const PresentExtras& ex) = 0;  // the per-tick draw
    virtual bool coversShell() const = 0;             // render true (uiAccess band) / transform false
};
}
