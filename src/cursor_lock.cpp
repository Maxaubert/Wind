#include "cursor_lock.h"
namespace wind {
void CursorLockController::toggle() { locked_ = !locked_; }
void CursorLockController::commitClick() { locked_ = false; }
void CursorLockController::reset()       { locked_ = false; }
}
