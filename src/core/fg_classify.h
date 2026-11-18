// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
// src/core/fg_classify.h
// Foreground-window classification split out of main.cpp (#7): the learned churny-app registry
// (issue #148, churny_apps.txt), exe-name lists, the transient-overlay and shell-desktop tests.
// Helpers that need the tick state stay in main.cpp.
#pragma once
#include <windows.h>
#include <string>
#include "config.h"

namespace wind {

// Churny-app registry (issue #148 trigger 3): learned apps whose cursor-shape churn resets the GPU
// driver under per-tick transform writes. Persisted in churny_apps.txt next to the log dir.
void LoadChurnyApps();
void MarkChurnyApp(const std::wstring& exe, const char* why);
bool IsChurnyFg(HWND fg);

// Lower-case bare exe file name of a window's process, or empty.
std::wstring ExeNameOf(HWND h);

// Is the foreground window's exe named in a comma-separated list (bare name, case-insensitive)?
bool FgExeInList(HWND fg, const std::string& list);

// An app on cfg.transformExclude never gets the transform engine.
bool IsTransformExcluded(HWND fg, const Config& cfg);

// Is the foreground a transient overlay or shell surface rather than a real app? Memoised per HWND.
bool IsOverlayFg(HWND fg);

// Shell desktop (Progman/WorkerW) in front, issue #172.
bool IsShellDesktopFg(HWND fg);

// Keyboard-hook suspension list (issue #156).
bool IsNoSwallowApp(HWND fg, const Config& cfg);

}  // namespace wind
