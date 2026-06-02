#include "magnifier_engine.h"
#include <windows.h>
#include <magnification.h>
namespace wind {
bool MagnifierEngine::initialize() {
    ready_ = MagInitialize() ? true : false;
    return ready_;
}
void MagnifierEngine::setTransform(double level, int xOffset, int yOffset, bool inputXform) {
    if (!ready_) return;
    MagSetFullscreenTransform(static_cast<float>(level), xOffset, yOffset);

    // inputXform=false (magInputTransform=0): do NOT remap input. MagSetInputTransform repositions
    // the system pointer into the magnified region; since the caller also re-centers the lens on
    // GetCursorPos every tick, that forms a feedback loop pinning the pointer to center and leaving
    // an unclickable dead band at every screen edge. With it off, GetCursorPos returns the raw cursor,
    // the lens follows it freely to the edges, and a click lands at the real cursor position. We must
    // still tear down any previously-set transform, so fall through to the (level<=1 || !inputXform)
    // reset branch below.
    const int sw = GetSystemMetrics(SM_CXSCREEN);
    const int sh = GetSystemMetrics(SM_CYSCREEN);
    if (inputXform && level > 1.0) {
        // PHYSICAL pixels for both rects, matching MagSetFullscreenTransform's offsets
        // and Microsoft's fullscreen magnifier sample. The OS feeds input in physical
        // screen coordinates (GetCursorPos spans the full physical resolution for this
        // per-monitor-aware process), so dest MUST be the full physical screen; a
        // smaller (e.g. DPI-"logical") dest leaves the bottom/right of the screen
        // outside the mapped region -> a dead band there. The DPI scale cancels in the
        // dest->src linear map anyway, so there is nothing to convert.
        RECT dest{ 0, 0, sw, sh };
        RECT src{ xOffset, yOffset,
                  xOffset + static_cast<LONG>(sw / level),
                  yOffset + static_cast<LONG>(sh / level) };
        MagSetInputTransform(TRUE, &src, &dest);   // no-op without UIAccess (harmless)
        inputTransformOn_ = true;
    } else if (inputTransformOn_) {
        RECT z{ 0, 0, sw, sh };
        MagSetInputTransform(FALSE, &z, &z);       // 1x: restore 1:1 input mapping
        inputTransformOn_ = false;
    }
}
void MagnifierEngine::clearInputTransform() {
    if (!ready_) return;
    RECT z{ 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
    MagSetInputTransform(FALSE, &z, &z);   // wipe any (possibly stale, cross-process) input remap
    inputTransformOn_ = false;
}
void MagnifierEngine::shutdown() {
    if (!ready_) return;
    if (inputTransformOn_) {                        // never leave input remapped
        RECT z{ 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
        MagSetInputTransform(FALSE, &z, &z);
        inputTransformOn_ = false;
    }
    MagSetFullscreenTransform(1.0f, 0, 0);  // never leave the screen zoomed
    MagUninitialize();
    ready_ = false;
}
}
