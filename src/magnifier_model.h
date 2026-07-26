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
    bool  gameFreeze = false;     // transform game session (issue #148): cursor frozen; sprite marks
                                  //   the aim point at the lens center
    bool  pauseWrites = false;    // skip transform writes this tick (serializes an injected cursor
                                  //   move so no write can race it - the proven TDR)
    // Game perf (issue #148; render model only, others ignore):
    bool  fsGame = false;         // foreground covers the monitor -> skip the periodic topmost backstop
    bool  forceCrop = false;      // fsGame && gameCrop: crop the capture copy to the magnified region
    bool  noVsync = false;        // game pacing engaged: Present(0,0); the main loop's timer paces
    bool  gatePresent = false;    // skip the frame while the previous present hasn't executed on the
                                  //   GPU (never block the main thread on a starved GPU)
    int   syncOverride = 0;       // 0 = cfg-driven; 2 = vblank-locked half-rate presents (game mode)
};
struct IMagnifierModel {
    virtual ~IMagnifierModel() = default;
    virtual bool initialize(const MonitorTarget& monitor) = 0;
    virtual void shutdown() = 0;
    virtual bool ready() const = 0;
    virtual void hideSystemCursor(bool hide) = 0;
    virtual void setActive(bool active) = 0;          // reveal/hide overlay, or enable/disable transform
    virtual void onActivate() {}
    // Called every tick while IDLE (not zoomed). The transform model uses it to expire its
    // rest-warm: keeping DWM's magnification pipeline hot forever taxes every composite
    // (field-reported hitching at 1x with MPO off), so it parks after a short cooldown.
    virtual void idleTick() {}                      // called on idle->active (render: invalidateCapture/prime)
    virtual bool retarget(const MonitorTarget& m) { (void)m; return false; }  // render-only; false = unchanged
    virtual void present(const MapResult& r, double level, const Config& cfg,
                         const MonitorTarget& mon, const PresentExtras& ex) = 0;  // the per-tick draw
    virtual bool coversShell() const = 0;             // whether the magnified view covers the shell
    virtual bool supportsInspect() const { return true; }  // magnify model: false (Magnifier owns
                                                           //   the view/cursor; no freeze+reticle)
    // Magnify model: the model drives its own zoom from raw held-direction and Wind's level
    // machinery (ZoomController, mapper, overlay activation) is bypassed entirely. RunTick calls
    // nativeZoomTick(dir) every tick (dir: +1 zoom-in held, -1 zoom-out held, 0 idle) and skips
    // the rest of the zoom pipeline when selfDrivenZoom() is true.
    virtual bool selfDrivenZoom() const { return false; }
    virtual void nativeZoomTick(int dir, const Config& cfg) { (void)dir; (void)cfg; }
};
}
