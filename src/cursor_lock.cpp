#include "cursor_lock.h"
namespace wind {
void CursorLockController::toggle(bool zoomedIn) {
    if (!zoomedIn) return;     // lock only applies while zoomed
    locked_ = !locked_;
}
void CursorLockController::commitClick() { locked_ = false; }
void CursorLockController::reset()       { locked_ = false; }
}
