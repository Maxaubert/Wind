// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
// src/core/single_instance.h
// The single-instance guard and the input-state heal net, split out of main.cpp (#7). Exactly one
// Wind.exe owns the named mutex; the owner restores the global cursor state on every exit path.
#pragma once
#include <windows.h>

namespace wind {

// Single-instance startup events route through the unified logger (category "startup").
void SiLog(const char* msg, unsigned long val);

// Force-restore any global input state a previous Wind may have left dirty. Idempotent.
void RestoreInputState();

// atexit net: restores only while this process owns the single-instance mutex.
void AtExitRestore();

// Records whether this process owns the single-instance mutex (read by AtExitRestore).
void MarkOwnsInstance(bool owns);

// Claims the single-instance mutex (signalling, then terminating, a busy predecessor). `mtx`
// receives the owned handle and `quitEvent` the quit-request event. False: refuse to start.
bool AcquireSingleInstance(HANDLE& mtx, HANDLE& quitEvent);

// Normal-exit hand-over: heal the input state while the mutex is still ours, then release it.
void ReleaseInstance(HANDLE mtx);

}  // namespace wind
