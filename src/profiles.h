// Pure profile logic (no <windows.h>): what belongs in a profile file vs the live magnifier.ini,
// profile-name validation, and the text transforms used on every switch/mirror. I/O lives in
// src/profiles_io.h (Win32) and the callers. See docs/specs/2026-08-12-profiles-design.md.
#pragma once
#include <string>
#include <vector>
namespace wind {
// Machine/app state that never travels with a profile: the active-profile pointer itself,
// the onboarding flag, the UI-only theme/palette/advanced toggles (uiTheme, uiPalette, showAdvanced), and the five tray-menu keys (#313).
bool IsGlobalProfileKey(const std::string& key);
// "" when the (already-trimmed) name is a valid profile name, else a short user-facing reason.
std::string ProfileNameError(const std::string& name);
// ASCII case-insensitive membership test against existing profile names.
bool ProfileNameTaken(const std::string& name, const std::vector<std::string>& names);
// Live ini text -> profile file text: global-key lines removed, every other line kept verbatim
// (comments and ordering survive, so profile files stay hand-editable like the live ini).
std::string MakeProfileText(const std::string& liveText);
// Switch transform: the profile's text (with any smuggled global-key lines stripped) plus the
// global-key lines carried over from the old live text, with profile=<name> set. An EMPTY profile
// text therefore yields a live ini holding only the globals: every profile-scoped key falls back
// to the built-in ParseConfig default (this is how "new profile = factory defaults" works).
std::string MakeLiveText(const std::string& profileText, const std::string& oldLiveText,
                         const std::string& name);
// "<base> copy", then "<base> copy 2", ...: first name not taken (case-insensitive). The base is
// truncated as needed so the result always passes ProfileNameError's 40-char cap.
std::string NextCopyName(const std::string& base, const std::vector<std::string>& names);
// ASCII case-insensitive profile-name equality (profile identity is case-insensitive everywhere:
// NTFS file names, the tray checkmark, ProfileNameTaken).
bool SameProfileName(const std::string& a, const std::string& b);
// "" when `text` is plausible profile-file content, else a short reason. Accepts empty or
// comment-only text (the legitimate factory-defaults profile) but rejects binary content (NUL
// bytes), absurd size (> 256 KB), and text that has non-comment lines yet parses to zero keys -
// so a corrupt file can never be silently applied as "factory defaults" on switch.
std::string ProfileTextError(const std::string& text);
// True when any profile-scoped key differs between the live session text and the saved profile
// text. Global keys are ignored; values are compared trimmed and numerically ("1.0" == "1"). A key
// missing on one side compares as the built-in default when the first-run template carries that key
// (so an explicit default is not a change), and as missing otherwise.
bool SessionDiffers(const std::string& liveText, const std::string& profileText);
// True for the per-process temp name WriteTextFileAtomic leaves behind when a process dies between
// its write and its rename: "<anything>.ini.<pid>.tmp" (ASCII case-insensitive). `pid` receives the
// number. Used by the stale-temp sweep (SweepStaleIniTmp in profiles_io.h).
bool ParseIniTmpName(const std::wstring& fileName, unsigned long& pid);
// UpdateIniText for profile-scoped keys only: a global key returns the input unchanged.
std::string UpdateProfileKey(const std::string& profileText, const std::string& key,
                             const std::string& value);
}
