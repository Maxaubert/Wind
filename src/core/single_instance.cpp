// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
// src/core/single_instance.cpp - see single_instance.h. Moved verbatim from main.cpp (#7).
#include "single_instance.h"
#include <tlhelp32.h>
#include <magnification.h>
#include <atomic>
#include "logging.h"
#include "mag_host.h"
#include "tray_ipc.h"

namespace wind {

// Force-restore any global input state a previous Wind may have left dirty (cursor hidden, cursor
// confined, a stuck show-count). Safe to call unconditionally at startup and at exit: every call
// is idempotent. This is the net that guarantees a force-killed or crashed predecessor can never
// leave the machine with a hidden/locked cursor - the next launch (and our own exit) heals it.
void RestoreInputState() {
    ClipCursor(nullptr);                                         // release any cursor confinement
    if (wind::MagApiAcquire()) { MagShowSystemCursor(TRUE); wind::MagApiRelease(); }   // un-hide the OS cursor
    SystemParametersInfoW(SPI_SETCURSORS, 0, nullptr, SPIF_SENDCHANGE);      // reload system cursors
    for (int i = 0; i < 8 && ShowCursor(TRUE) < 0; ++i) {}       // bump our show-count back to visible
}

// Minimal restore for abnormal CRT exit (atexit / ExitProcess). Non-blocking: must not touch the
// hook thread (its stop() waits, which could hang during teardown); the hook dies with the process
// and only swallows side-buttons anyway. The damaging state (hidden/confined cursor) is undone here.
// Only while we own the single-instance mutex: a refused or lingering second process must not heal
// global state under the live owner, and a normal exit restores BEFORE releasing the mutex (see
// ReleaseInstance), so a successor's tint capture never sees a half-restored scheme.
static std::atomic<bool> g_ownsInstance{false};
void AtExitRestore() { if (g_ownsInstance.load()) RestoreInputState(); }
void MarkOwnsInstance(bool owns) { g_ownsInstance.store(owns); }

// Single-instance startup events route through the unified logger (category "startup").
void SiLog(const char* msg, unsigned long val) {
    wind::Log(wind::LogLevel::Info, "startup", "%s %lu", msg, val);
}

// Force-kill every OTHER Wind.exe (best effort; OpenProcess may be denied across integrity levels).
static void TerminateOtherWind() {
    const DWORD self = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W pe{ sizeof(pe) };
    if (Process32FirstW(snap, &pe)) {
        do {
            if (pe.th32ProcessID != self && _wcsicmp(pe.szExeFile, L"Wind.exe") == 0) {
                HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
                SiLog("terminate pid", pe.th32ProcessID);
                if (h) { BOOL ok = TerminateProcess(h, 0); SiLog("  terminate ok", ok);
                         if (!ok) SiLog("  terminate err", GetLastError()); CloseHandle(h); }
                else SiLog("  openprocess err", GetLastError());
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
}

// Guarantee EXACTLY ONE Wind.exe via a named mutex we OWN for our lifetime. This is the canonical,
// integrity-independent guard: the kernel auto-releases the mutex when its owner dies (even on a
// hard kill -> the next waiter gets WAIT_ABANDONED), so a zombie can NEVER permanently block a
// relaunch. `mtx` receives the owned handle. Returns false only when another LIVE instance will not
// yield - the caller then exits WITHOUT installing hooks, because two instances = two mouse hooks +
// two cursor-warp loops = a system-wide input lock. (The previous kill-only version failed here:
// TerminateProcess is blocked across the signed UIAccess build's integrity, and it had removed the
// refuse-to-start backstop, so a second instance proceeded anyway and locked input.)
//
// quitEvent is created here, as soon as the mutex is ours, so a successor that finds us busy can
// always signal it (it used to exist only after the whole startup). A null mutex fails closed.
bool AcquireSingleInstance(HANDLE& mtx, HANDLE& quitEvent) {
    mtx = CreateMutexW(nullptr, FALSE, wind::kInstanceMutexName);
    SiLog("createmutex err", GetLastError());
    if (!mtx) { SiLog("mutex null - REFUSING TO START (cannot guard against a second instance)", 0); return false; }
    // Quit-request channel for WindConfig.exe (onboarding close) and for a successor Wind. A window
    // message can't be used: the deployed Wind.exe is UIAccess, and UIPI silently blocks PostMessage
    // from the non-UIAccess WindConfig. A named event is a kernel object (not gated by UIPI) and
    // both run as the same user in the same session. Auto-reset, initially unsignaled.
    auto makeQuitEvent = [&] { quitEvent = CreateEventW(nullptr, FALSE, FALSE, wind::kQuitEventName); };
    DWORD w = WaitForSingleObject(mtx, 0);   // WAIT_ABANDONED = prior owner died holding it -> ours now
    if (w == WAIT_OBJECT_0 || w == WAIT_ABANDONED) { makeQuitEvent(); SiLog("acquired immediately w", w); return true; }
    SiLog("busy - signaling quit, w", w);
    HANDLE ev = OpenEventW(EVENT_MODIFY_STATE, FALSE, wind::kQuitEventName);
    if (ev) { SetEvent(ev); CloseHandle(ev); SiLog("quit event set", 0); }
    else SiLog("quit event open err", GetLastError());
    w = WaitForSingleObject(mtx, 3000);                       // wait for it to release on clean exit
    if (w == WAIT_OBJECT_0 || w == WAIT_ABANDONED) { makeQuitEvent(); SiLog("acquired after quit w", w); return true; }
    SiLog("still busy after quit - terminating, w", w);
    TerminateOtherWind();                                     // fallback: kill the straggler
    w = WaitForSingleObject(mtx, 2000);
    if (w == WAIT_OBJECT_0 || w == WAIT_ABANDONED) { makeQuitEvent(); SiLog("acquired after terminate w", w); return true; }
    SiLog("REFUSING TO START - another instance alive, w", w);
    CloseHandle(mtx); mtx = nullptr;
    return false;                                             // never stack a second hook/cursor loop
}

// Normal-exit hand-over: heal the input state while the mutex is still ours, then release it.
void ReleaseInstance(HANDLE mtx) {
    if (!mtx) return;
    RestoreInputState();
    g_ownsInstance.store(false);
    ReleaseMutex(mtx);
    CloseHandle(mtx);
}

}  // namespace wind
