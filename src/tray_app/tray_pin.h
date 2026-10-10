#pragma once
// "Pin to taskbar" (#436): keeps WindTray's notification-area icon on the taskbar next to the clock
// instead of in the hidden-icons overflow. Windows stores the choice per icon under
// HKCU\Control Panel\NotifyIconSettings\<id> (IsPromoted: 1 = taskbar, 0 = overflow); see
// docs/architecture/09-settings-ui.md. The matching logic below is pure (no <windows.h>) and tested.
#include <functional>
#include <string>

namespace wind { namespace TrayPin {

// Maps a braced known-folder GUID such as "{6D809377-6AF0-444B-8957-A3773F02200E}" to its folder path.
using GuidResolver = std::function<bool(const std::wstring& guid, std::wstring& dirOut)>;

// "{GUID}\rest" -> "<folder>\rest"; any other text is returned unchanged. A leading GUID the resolver
// cannot map gives an empty string (an entry we cannot place never matches).
std::wstring ExpandPath(const std::wstring& raw, const GuidResolver& resolve);

// Case-insensitive path equality; slash and backslash are the same, a trailing separator is ignored.
bool SamePath(const std::wstring& a, const std::wstring& b);

// True when a NotifyIconSettings entry (its ExecutablePath and UID values) belongs to this process's icon.
bool EntryMatches(const std::wstring& rawExePath, unsigned long entryUid,
                  const std::wstring& selfExePath, unsigned long selfUid, const GuidResolver& resolve);

enum class Result { Applied, AlreadyOk, NoEntry, Error };

// Registry side (tray_pin.cpp, Windows only). Writes IsPromoted on our own entry only when it differs.
// NoEntry: Explorer has not created the entry yet (first run, right after NIM_ADD); the caller retries.
Result ApplyPinned(bool pinned, unsigned long uid);

}}  // namespace wind::TrayPin
