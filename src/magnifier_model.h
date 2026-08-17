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
    bool  pauseWrites = false;    // skip transform writes this tick (serializes an Inspect click's
                                  //   injected absolute move so no write can race it - the proven
                                  //   TDR class, issue #148)
    // Game perf (issue #148; render model only, others ignore):
    // Drag-follow (issue #169): a mouse button is held in a FREE render session, so the pointer IS
    // the interaction (window drag / text selection). The render model must NOT weld it to the lens
    // centre this tick - the weld would fight the hand and the dragged content flickers between the
    // two positions. The lens follows the pointer instead (RunTick feeds unscaled deltas).
    bool  suppressCursorSync = false;
    // The mouse hook owns transform writes this tick (issue #206). SINGLE WRITER: two writers
    // sampling the cursor at different instants alternate between two positions at tick rate,
    // which is the wobble class #205 removed. The tick still triggers writes, but through the
    // hook's own function so there is one formula.
    bool  suppressTransformWrite = false;
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
    virtual void onActivate() {}                      // called on idle->active (render: invalidateCapture/prime)
    // Called every tick while IDLE (not zoomed, no Inspect). The transform model releases its
    // magnification context here: a live context keeps DWM in magnification-aware compositing,
    // where every cursor change an app makes costs a re-composite (issue #148).
    virtual void idleTick() {}
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
