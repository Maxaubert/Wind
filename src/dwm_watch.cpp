#include "dwm_watch.h"
#include <windows.h>
#include <tlhelp32.h>
#include <atomic>
#include "logging.h"

namespace wind {
namespace {
std::atomic<unsigned long> g_gen{0};
std::atomic<bool> g_started{false};

// This session's dwm.exe, or 0 while none runs.
unsigned long FindDwmPid(DWORD session) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{}; pe.dwSize = sizeof(pe);
    unsigned long pid = 0;
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        if (_wcsicmp(pe.szExeFile, L"dwm.exe") != 0) continue;
        DWORD s = 0;
        if (ProcessIdToSessionId(pe.th32ProcessID, &s) && s == session) { pid = pe.th32ProcessID; break; }
    }
    CloseHandle(snap);
    return pid;
}
}  // namespace

void StartDwmWatch() {
    if (g_started.exchange(true)) return;
    HANDLE th = CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
        SetThreadDescription(GetCurrentThread(), L"Wind DWM watch");
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        DWORD session = 0;
        ProcessIdToSessionId(GetCurrentProcessId(), &session);
        unsigned long last = FindDwmPid(session);
        for (;;) {
            Sleep(1000);
            const unsigned long pid = FindDwmPid(session);
            if (DwmRestarted(last, pid)) {
                const unsigned long gen = g_gen.fetch_add(1, std::memory_order_release) + 1;
                Log(LogLevel::Info, "dwm", "dwm.exe restarted (pid %lu -> %lu), generation %lu",
                    last, pid, gen);
            }
            if (pid != 0) last = pid;
        }
    }, nullptr, 0, nullptr);
    if (th) CloseHandle(th);
    else g_started.store(false);
}

unsigned long DwmGeneration() { return g_gen.load(std::memory_order_acquire); }
}  // namespace wind
