// Pure profile logic (no <windows.h>): what belongs in a profile file vs the live magnifier.ini,
// profile-name validation, and the text transforms used on every switch/mirror. I/O lives in
// src/profiles_io.h (Win32) and the callers. See docs/superpowers/specs/2026-08-12-profiles-design.md.
#pragma once
#include <string>
#include <vector>
namespace wind {
// Machine/app state that never travels with a profile: the active-profile pointer itself,
// the onboarding flag, and the UI-only theme/advanced toggles.
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
// "<base> copy", then "<base> copy 2", ...: first name not taken (case-insensitive).
std::string NextCopyName(const std::string& base, const std::vector<std::string>& names);
}
