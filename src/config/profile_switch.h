// Profile switching and "relaunch Wind, put the model back if that fails", shared by the tray
// (WindTray.exe) and the settings host (WindConfig.exe). Win32: MUST be included AFTER <windows.h>.
// Each surface keeps only its own messages and logging; the steps and their order live here once.
#pragma once
#include <shellapi.h>
#include <string>
#include "config.h"          // ParseConfig: the canonical model of a text
#include "config_path.h"     // ExeDir
#include "ini_edit.h"        // UpdateIniText
#include "profiles.h"        // MakeLiveText, ProfileTextError
#include "profiles_io.h"     // IniWriteLock, ReadLiveIni, WriteTextFileAtomic, SessionKeepPath

namespace wind {

// session.keep tells the next Wind start to keep the unsaved session (see ResetSessionToProfile).
// Written right before a restart Wind's own tools trigger, removed again when that restart fails.
inline void WriteSessionKeep() {
    HANDLE h = CreateFileW(SessionKeepPath().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
}
inline void ClearSessionKeep() { DeleteFileW(SessionKeepPath().c_str()); }

// Launch the Wind.exe next to this exe (never our own exe: this runs in WindTray and WindConfig).
// Launching while Wind is already running is how a restart works: the new instance finds the
// single-instance mutex held, sets the quit event and waits for the incumbent to exit cleanly. A
// shell launch, so the child gets an ordinary token. Returns ShellExecuteW's result (success is
// > 32), or 0 when Wind.exe is missing.
inline INT_PTR LaunchWindExe() {
    const std::wstring exe = ExeDir() + L"\\Wind.exe";
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) return 0;
    return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", exe.c_str(), nullptr,
                                                   ExeDir().c_str(), SW_SHOWNORMAL));
}

// A relaunch for a model change failed: keep "ini model == running model" by writing the old model
// back (an empty one means the key was unset, which reads as hybrid) and dropping session.keep.
// Re-reads the live ini, since another surface may have written since; `fallbackLive` (may be
// null) is used when that read fails.
inline void RevertModelAfterFailedRelaunch(const std::wstring& ini, const std::string& oldModel,
                                           const std::string* fallbackLive) {
    ClearSessionKeep();
    IniWriteLock iniLock;   // review item 71; a mutex, so a caller already holding it is fine
    std::string cur;
    if (!ReadLiveIni(ini, cur)) {
        if (!fallbackLive) return;
        cur = *fallbackLive;
    }
    WriteTextFileAtomic(ini, UpdateIniText(cur, "model", oldModel.empty() ? "hybrid" : oldModel));
}

enum class ProfileSwitchError {
    None,
    FileMissing,      // the profile's file does not exist
    ReadFailed,       // the profile's file exists but could not be read
    Corrupt,          // ProfileTextError rejected it (`reason` holds the text)
    LiveUnreadable,   // the live ini could not be read
    WriteFailed,      // the live ini could not be written
};
struct ProfileSwitchResult {
    ProfileSwitchError error = ProfileSwitchError::None;
    std::string reason;          // ProfileTextError's text when error == Corrupt
    DWORD lastError = 0;         // GetLastError() of the failed step
    std::string oldModel, newModel;
    bool relaunched = false;     // the model changed on a single-engine Wind: Wind.exe was launched
    bool relaunchFailed = false; // ...and that failed, so the model was put back (the switch stays)
    INT_PTR launchRc = 0;        // LaunchWindExe's result when relaunched
};

// Switch the live ini to the profile in `profPath` (named `nameUtf8`), globals preserved. The
// core's dir-watch hot-reloads everything, `model` included when both engines are up (running.model
// "live"). With one engine left `model` is read once at launch, so a model change relaunches Wind
// (session.keep first, so the restart keeps the session) and puts the old model back if that fails.
//
// A read failure never masquerades as an empty profile (empty means factory defaults by design), and
// corrupt content is never applied. `mirrorOutgoing` first captures the live ini into the OUTGOING
// profile (hand edits made via "Edit config file" would otherwise be lost); a create-from-current
// passes false, because its unsaved changes belong to the new profile.
inline ProfileSwitchResult SwitchProfile(const std::wstring& ini, const std::wstring& profPath,
                                         const std::string& nameUtf8, bool mirrorOutgoing) {
    ProfileSwitchResult r;
    if (GetFileAttributesW(profPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        r.error = ProfileSwitchError::FileMissing;
        return r;
    }
    std::string profText;
    if (!ReadTextFileOk(profPath, profText)) {
        r.lastError = GetLastError();
        r.error = ProfileSwitchError::ReadFailed;
        return r;
    }
    r.reason = ProfileTextError(profText);
    if (!r.reason.empty()) {
        r.error = ProfileSwitchError::Corrupt;
        return r;
    }
    IniWriteLock iniLock;   // held to the end, rollback included (review item 71)
    std::string oldLive;
    if (!ReadLiveIni(ini, oldLive)) {
        r.lastError = GetLastError();
        r.error = ProfileSwitchError::LiveUnreadable;
        return r;
    }
    if (mirrorOutgoing) MirrorLiveToActiveProfile(ini, oldLive);
    const std::string newLive = MakeLiveText(profText, oldLive, nameUtf8);
    // WriteTextFileAtomic checks the write and the replace itself; its result is the verdict (a
    // re-read can fail on its own and would report a successful switch as a failed one).
    if (!WriteTextFileAtomic(ini, newLive)) {
        r.lastError = GetLastError();
        r.error = ProfileSwitchError::WriteFailed;
        return r;
    }
    // ParseConfig canonicalizes (unknown or retired models -> hybrid), so every surface agrees on
    // whether the model changed.
    r.oldModel = ParseConfig(oldLive).model;
    r.newModel = ParseConfig(newLive).model;
    r.relaunched = r.oldModel != r.newModel && !EngineSwitchesLive();
    if (r.relaunched) {
        WriteSessionKeep();   // our own restart: the start must not reset the session
        r.launchRc = LaunchWindExe();
        if (r.launchRc <= 32) {
            r.relaunchFailed = true;
            RevertModelAfterFailedRelaunch(ini, r.oldModel, &newLive);
        }
    }
    return r;
}

}  // namespace wind
