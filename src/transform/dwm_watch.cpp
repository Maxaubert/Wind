// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary and confidential source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
#include "dwm_watch.h"
#include <windows.h>
#include <wtsapi32.h>
#include <atomic>
#include "logging.h"

#pragma comment(lib, "wtsapi32.lib")

namespace wind {
namespace {
std::atomic<unsigned long> g_gen{0};
std::atomic<bool> g_started{false};
HANDLE g_stop = nullptr;     // signalled by StopDwmWatch
HANDLE g_thread = nullptr;

// This session's dwm.exe, or 0 while none runs. The WTS list carries each process's session, so
// dwm.exe is never opened: ProcessIdToSessionId on it fails with access denied, which kept this
// watch from ever seeing a restart (#458).
unsigned long FindDwmPid(DWORD session) {
    WTS_PROCESS_INFOW* list = nullptr;
    DWORD n = 0;
    if (!WTSEnumerateProcessesW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &list, &n) || !list) return 0;
    unsigned long pid = 0;
    for (DWORD i = 0; i < n; ++i) {
        if (list[i].SessionId == session && list[i].pProcessName &&
            _wcsicmp(list[i].pProcessName, L"dwm.exe") == 0) { pid = list[i].ProcessId; break; }
    }
    WTSFreeMemory(list);
    return pid;
}
}  // namespace

void StartDwmWatch() {
    if (g_started.exchange(true)) return;
    g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE th = CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
        SetThreadDescription(GetCurrentThread(), L"Wind DWM watch");
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        DWORD session = 0;
        ProcessIdToSessionId(GetCurrentProcessId(), &session);
        unsigned long last = FindDwmPid(session);
        for (;;) {
            // The stop event doubles as the 1 s poll interval: Wind exit ends the thread at once.
            if (WaitForSingleObject(g_stop, 1000) == WAIT_OBJECT_0) break;
            const unsigned long pid = FindDwmPid(session);
            if (DwmRestarted(last, pid)) {
                const unsigned long gen = g_gen.fetch_add(1, std::memory_order_release) + 1;
                Log(LogLevel::Info, "dwm", "dwm.exe restarted (pid %lu -> %lu), generation %lu",
                    last, pid, gen);
            }
            if (pid != 0) last = pid;
        }
        return 0;
    }, nullptr, 0, nullptr);
    if (th) g_thread = th;
    else { CloseHandle(g_stop); g_stop = nullptr; g_started.store(false); }
}

void StopDwmWatch() {
    if (!g_thread) return;
    SetEvent(g_stop);
    WaitForSingleObject(g_thread, 1000);   // a snapshot walk in flight finishes well inside this
    CloseHandle(g_thread); g_thread = nullptr;
    CloseHandle(g_stop); g_stop = nullptr;
    g_started.store(false);
}

unsigned long DwmGeneration() { return g_gen.load(std::memory_order_acquire); }
}  // namespace wind
