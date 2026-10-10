#include "render_model.h"

namespace wind {

RenderModel::RenderModel(int zorderBand, bool hdrTonemap, int gpuPriority)
    : zorderBand_(zorderBand), hdrTonemap_(hdrTonemap), gpuPriority_(gpuPriority) {}

bool RenderModel::initialize(const MonitorTarget& m) {
    return engine_.initialize(m, zorderBand_, hdrTonemap_, gpuPriority_);
}
void RenderModel::shutdown() { engine_.shutdown(); }
bool RenderModel::ready() const { return engine_.ready(); }
void RenderModel::hideSystemCursor(bool hide) { engine_.hideSystemCursor(hide); }
void RenderModel::setActive(bool active) {
    visible_ = active;
    engine_.setVisible(active);
    // Zoom-out: also drop the Desktop Duplication session (issue #148). While a duplication is
    // alive, DWM keeps servicing it; idle at 1x should cost the system nothing. The next zoom-in
    // recreates it anyway (onActivate -> invalidateCapture always forces a fresh grab), so this
    // changes nothing about activation behavior.
    if (!active) engine_.invalidateCapture();
}
void RenderModel::onActivate() {   // reveal/prime stays in main loop (needs ForegroundCoversMonitor)
    engine_.invalidateCapture();
    engine_.armRevealFence();      // gate the reveal on this session's first Present executing (#140)
}
bool RenderModel::retarget(const MonitorTarget& m) { return engine_.retarget(m); }
bool RenderModel::coversShell() const { return true; }
RenderEngine& RenderModel::engine() { return engine_; }
bool RenderModel::deviceLost() const { return engine_.deviceLost(); }
void RenderModel::pollDeviceRemoved() { engine_.pollDeviceRemoved(); }
bool RenderModel::recoverDeviceLost() { return engine_.recoverDeviceLost(); }
void RenderModel::primeReveal() { engine_.primeReveal(); }
bool RenderModel::frameCompositedSincePrime() const { return engine_.frameCompositedSincePrime(); }
bool RenderModel::revealFrameDone(double spinBudgetMs) { return engine_.revealFrameDone(spinBudgetMs); }
void RenderModel::invalidateCapture() { engine_.invalidateCapture(); }
bool RenderModel::waitVBlank() { return engine_.waitVBlank(); }

void RenderModel::present(const MapResult& r, double level, const Config& cfg,
                          const MonitorTarget& mon, const PresentExtras& ex) {
    RenderFrameParams p{};
    FillRenderParams(p, r, cfg, mon, level);
    p.outline = ex.outline;
    p.outlineAlpha = ex.outlineAlpha;
    p.cursorLocked = ex.cursorLocked;
    p.cursorMode = ex.cursorMode;
    if (ex.clickOverride) { p.clickDesktopX = ex.clickDesktopX; p.clickDesktopY = ex.clickDesktopY; }
    p.suppressCursorSync = ex.suppressCursorSync;   // mid-drag: the pointer owns the interaction (#169)
    p.colorOn = ex.colorOn;                     // colour filter in the shader (#288)
    p.color = ex.color;
    p.fsGame = ex.fsGame;                       // skip the periodic topmost backstop over a game
    if (ex.forceCrop) p.cropCapture = true;     // game session: crop the copy to the magnified view
    if (ex.noVsync)   p.vsync = false;          // game pacing: timer paces, Present(0,0)
    p.gatePresent = ex.gatePresent;             // never block the tick behind an in-flight present
    p.syncOverride = ex.syncOverride;           // 2 = steady half-rate vblank lock (game mode)
    engine_.renderFrame(p);
}
}
