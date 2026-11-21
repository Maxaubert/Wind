// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
// Pure profile logic (no <windows.h>): what belongs in a profile file vs the live magnifier.ini,
// profile-name validation, and the text transforms used on every switch/mirror. I/O lives in
// src/config/profiles_io.h (Win32) and the callers. See docs/specs/2026-08-12-profiles-design.md.
#pragma once
#include <map>
#include <string>
#include <vector>
namespace wind {
// THE list of UI-only keys: the core never reads them, so a change to one never reloads the core
// (StripUiOnlyKeys). The onboarding flag, the theme/palette/advanced toggles, the five tray-menu keys
// (#313), trayPinned (#436), uiHighResNoticeOff (#441) and the trayInProfiles marker. All but the
// tray-menu keys are also global (never in a profile).
inline constexpr const char* kUiOnlyKeys[] = {
    "onboarded", "uiTheme", "uiPalette", "showAdvanced", "trayPerf", "traySliders", "traySliderOrder",
    "trayToggles", "trayToggleOrder", "trayPinned", "uiHighResNoticeOff", "trayInProfiles",
};
// Live-ini marker: the one-time copy of the live tray menu into the profiles (AddMissingTrayKeys in
// MigrateIniFiles) has run. Without it, a profile on the default layout would pick up another
// profile's layout at every start.
inline constexpr const char* kTrayInProfilesKey = "trayInProfiles";
// The UI-only keys a profile DOES save (2026-11-21): the tray-menu layout. Editing the tray menu
// lights Save, and a profile switch brings that profile's tray menu.
inline constexpr const char* kProfileUiKeys[] = {
    "trayPerf", "traySliders", "traySliderOrder", "trayToggles", "trayToggleOrder",
};
// Session-only screen colour (2026-11-21): the core reads them, but they are never saved in a
// profile, never count as unsaved, keep their value across a profile switch, and go back to the
// template default at every plain Wind start (ResetSessionKeys).
inline constexpr const char* kSessionKeys[] = { "colorWarmPct", "colorDimPct", "colorSatPct" };
// Keys that never travel with a profile: `profile` itself, the UI-only keys outside kProfileUiKeys,
// and kSessionKeys. tests/fixtures/global_keys.txt holds the same set (the UI's GLOBAL_KEYS in
// ui/src/session.js is tested against it); every C++ site goes through this function.
bool IsGlobalProfileKey(const std::string& key);
// Live ini text with every kSessionKeys key that the text sets put back to its template default.
// Keys the text does not set stay absent (they read as the default already).
std::string ResetSessionKeys(const std::string& liveText);
// "" when the (already-trimmed) name is a valid profile name, else a short user-facing reason.
std::string ProfileNameError(const std::string& name);
// ASCII case-insensitive membership test against existing profile names.
bool ProfileNameTaken(const std::string& name, const std::vector<std::string>& names);
// Live ini text -> profile file text: global-key lines removed, every other line kept verbatim
// (comments and ordering survive, so profile files stay hand-editable like the live ini).
std::string MakeProfileText(const std::string& liveText);
// Switch transform: the profile's text (with any global-key lines stripped) plus the global-key
// lines carried over from the old live text, with profile=<name> set. An EMPTY profile
// text therefore yields a live ini holding only the globals: every profile-scoped key falls back
// to the built-in ParseConfig default (this is how "new profile = factory defaults" works).
std::string MakeLiveText(const std::string& profileText, const std::string& oldLiveText,
                         const std::string& name);
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
// The five tray-menu keys as an ini without them reads (src/tray/tray_items.cpp ParseTrayLayout).
const std::map<std::string, std::string>& TrayLayoutDefaults();
// The tray-menu keys the live ini sets, added to a profile text that has none. Run once, on every
// profile (MigrateIniFiles, gated by kTrayInProfilesKey): tray layouts were global before 2026-11-21,
// so an older profile gets the layout the user already has instead of the default one on a switch.
std::string AddMissingTrayKeys(const std::string& profileText, const std::string& liveText,
                               bool* changed = nullptr);
// True for the per-process temp name WriteTextFileAtomic leaves behind when a process dies between
// its write and its rename: "<anything>.ini.<pid>.tmp" (ASCII case-insensitive). `pid` receives the
// number. Used by the stale-temp sweep (SweepStaleIniTmp in profiles_io.h).
bool ParseIniTmpName(const std::wstring& fileName, unsigned long& pid);
// UpdateIniText for profile-scoped keys only: a global key returns the input unchanged.
std::string UpdateProfileKey(const std::string& profileText, const std::string& key,
                             const std::string& value);
}
