#include "magnifier_engine.h"
#include <windows.h>
#include <magnification.h>
namespace wind {
bool MagnifierEngine::initialize() {
    ready_ = MagInitialize() ? true : false;
    return ready_;
}
void MagnifierEngine::setTransform(double level, int xOffset, int yOffset) {
    if (!ready_) return;
    MagSetFullscreenTransform(static_cast<float>(level), xOffset, yOffset);

    // Route mouse input to the magnified element. Since Windows 10 1703, without this
    // the OS sends clicks/hit-tests to the *unmagnified* coordinates, so while zoomed
    // the cursor sits over the magnified target visually but input lands elsewhere -
    // small targets (text fields) miss and show no I-beam. Keep the input rectangle
    // matched to the visual transform on every update so they never diverge.
    //
    // MagSetInputTransform requires UIAccess; without it the call fails
    // (ERROR_ACCESS_DENIED) and is a harmless no-op, leaving behavior as before.
    const int sw = GetSystemMetrics(SM_CXSCREEN);
    const int sh = GetSystemMetrics(SM_CYSCREEN);
    if (level > 1.0) {
        RECT dest{ 0, 0, sw, sh };                 // where the magnified view is shown
        RECT src{ xOffset, yOffset,                // the region being magnified
                  xOffset + static_cast<int>(sw / level),
                  yOffset + static_cast<int>(sh / level) };
        inputTransformOk_ = (MagSetInputTransform(TRUE, &src, &dest) != 0);
        inputTransformOn_ = true;
    } else if (inputTransformOn_) {
        RECT z{ 0, 0, sw, sh };
        MagSetInputTransform(FALSE, &z, &z);       // 1x: restore 1:1 input mapping
        inputTransformOn_ = false;
    }
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
