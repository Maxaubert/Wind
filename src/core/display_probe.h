// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
// src/core/display_probe.h
// Display and foreground probes split out of main.cpp (#7): refresh rate and rotation, the
// monitor under the cursor, the fullscreen-cover test, the virtual-desktop bounds, and the
// game-inspect focus stealer (issue #144). Thin Win32 reads; no tick state.
#pragma once
#include <windows.h>
#include "render_engine.h"   // MonitorTarget

namespace wind {

// Whether the output is rotated to portrait (90/270).
bool DetectRotatedOutput(const wchar_t* device);

// Refresh rate (Hz) of a display (GDI device name); nullptr/empty = the primary. 0 when the query
// fails, so a caller can tell a failure from a real 60Hz panel.
int DetectRefreshHz(const wchar_t* device = nullptr);

// The primary monitor as a MonitorTarget; the universal fallback.
MonitorTarget PrimaryMonitor();

// The monitor the cursor is on, or the primary if the query fails.
MonitorTarget MonitorUnderCursor();

// Whether two targets are the same monitor (origin + size + device name).
bool SameMonitor(const MonitorTarget& a, const MonitorTarget& b);

// True when the foreground window covers the whole target monitor (fullscreen/borderless app).
bool ForegroundCoversMonitor(const MonitorTarget& mon);

// The invisible game-inspect focus-steal helper window (issue #144). Defined once, in
// display_probe.cpp; the tick loop reads it to tell when the helper holds foreground.
extern HWND g_focusStealer;
HWND EnsureFocusStealer(const MonitorTarget& mon);

// SetForegroundWindow with a verified result and the AttachThreadInput fallback.
bool StealForeground(HWND w);

// Virtual-desktop bounds (the union of all monitors), used per zoomed frame to detect a game
// clipping the cursor. Cached because GetSystemMetrics is a syscall and these bounds change only
// on a display-topology change; refreshed on each zoom-in (where we also retarget the monitor).
struct VirtualBounds { int x, y, w, h; };
VirtualBounds QueryVirtualBounds();

}  // namespace wind
