// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
#include "cursor_lock.h"
namespace wind {
void CursorLockController::toggle() { locked_ = !locked_; }
void CursorLockController::reset()  { locked_ = false; }
}
