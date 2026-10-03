// src/sched_priority.h - keep the tick loop on time when the rest of the PC is busy (issue #334).
//
// The tick thread ran at normal priority, so under heavy background CPU load (renders, builds) it
// queued behind every other normal thread and a zoom in/out froze for a few frames (traces: 139-737 ms
// stalls, only while the machine was saturated). Wind also never owns a foreground window, so Windows
// may power-throttle it as a background process.
//
// THREAD_PRIORITY_HIGHEST, not MMCSS or the realtime band: one step above normal work is enough to win
// against background load, and it stays below DWM, so the tick can never delay composition. The loop
// sleeps at 1x and waits on a timer when zoomed, so the raised priority costs nothing while idle.
#pragma once
#include <windows.h>

namespace wind {

// Call once on the tick thread, before the loop.
inline void RaiseTickThreadPriority() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
}

// Never run Wind in EcoQoS / efficiency mode, and honour its timer resolution even with no visible window.
inline void OptOutOfPowerThrottling() {
    PROCESS_POWER_THROTTLING_STATE s{};
    s.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    s.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
    s.StateMask = 0;   // controlled bits off = never throttled
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &s, sizeof(s));
}

}  // namespace wind
