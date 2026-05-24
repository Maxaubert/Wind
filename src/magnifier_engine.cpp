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
}
void MagnifierEngine::shutdown() {
    if (!ready_) return;
    MagSetFullscreenTransform(1.0f, 0, 0);  // never leave the screen zoomed
    MagUninitialize();
    ready_ = false;
}
}
