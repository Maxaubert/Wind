// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
// src/core/tick_rate.h
// Pure (no <windows.h>): scales a 144Hz-tuned tick count to the loop's real tick rate. Moved out
// of main.cpp (#7). Unit-tested in tests/test_tick_rate.cpp.
#pragma once

namespace wind {

// Small tick windows were field-tuned in TICKS on the 144Hz dev rig; a tick is one refresh, so
// the same count is 2.4x longer in real time at 60Hz and 0.6x at 240Hz (issue #223). This keeps
// their real-time duration by scaling the 144Hz-tuned count to the loop's actual tick rate.
inline int TicksAtHz(int base144, int hz) {
    if (hz <= 0) hz = 144;
    const int t = (base144 * hz + 72) / 144;
    return t < 1 ? 1 : t;
}

}  // namespace wind
