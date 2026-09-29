#pragma once
// Wind.exe's side of the tray split (issue #291). Wind no longer owns a tray icon: it creates the
// shared status block (tray_ipc.h) and keeps WindTray.exe running next to it. See
// docs/superpowers/specs/2026-09-29-tray-process-design.md.
#include <string>

namespace wind {
struct TrayShared;
namespace TrayHost {

// Creates Local\Wind_TrayState_v1, stamps it with our PID, and starts the supervisor thread that
// launches WindTray.exe from appDir (restarting it if it exits, at most 3 times a minute). Returns
// the mapped block, or nullptr if the mapping failed (Wind still runs; the tray just has no data).
TrayShared* Start(const std::wstring& appDir);

// Stops the supervisor. WindTray is NOT killed: it waits on our process handle and removes its
// icon the moment we exit.
void Stop();

}  // namespace TrayHost
}  // namespace wind
