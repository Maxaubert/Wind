#include "magnify_model.h"
#include "logging.h"
#include <windows.h>
#include <magnification.h>
#include <shellapi.h>
#include <fstream>

namespace wind {
namespace {

const wchar_t* kMagKey = L"Software\\Microsoft\\ScreenMagnifier";
// The values we modify at initialize (and therefore snapshot + restore on shutdown). We
// deliberately do NOT touch ZoomIncrement or FollowMouse: the whole point of this design is
// that Magnifier behaves exactly as the user configured it in Windows Settings.
const wchar_t* kSnapshotValues[] = { L"Magnification", L"MagnificationMode",
                                     L"MagnifierUIWindowMinimized" };

// Wheel-notch cadence. Measured (probe 7): 60 ms notches register 1:1 with no backlog and the
// view settles ~150 ms after the last notch. Faster is unmeasured; slower feels sluggish.
const unsigned long long kNotchIntervalMs = 60;

int ReadMagDword(const wchar_t* name, int fallback) {
    DWORD v = 0, cb = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, kMagKey, name, RRF_RT_REG_DWORD, nullptr, &v, &cb) == ERROR_SUCCESS)
        return (int)v;
    return fallback;
}

void WriteMagDword(const wchar_t* name, int value) {
    DWORD v = (DWORD)value;
    RegSetKeyValueW(HKEY_CURRENT_USER, kMagKey, name, REG_DWORD, &v, sizeof(v));
}

// Magnifier's main window class (present whenever Magnify.exe is running, fullscreen mode included).
bool MagnifierWindowPresent() { return FindWindowW(L"MagUIClass", nullptr) != nullptr; }

// %LOCALAPPDATA%\Wind\magnifier_backup.ini (the same per-user dir the ini fallback and logs use;
// never next to the exe - Program Files is read-only for the non-admin runtime).
std::wstring ResolveBackupPath() {
    wchar_t base[MAX_PATH]{};
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH) || !base[0]) return L"";
    std::wstring dir = std::wstring(base) + L"\\Wind";
    CreateDirectoryW(dir.c_str(), nullptr);   // idempotent
    return dir + L"\\magnifier_backup.ini";
}

// Inject a Win+<vk> chord (used only for Win+Esc = quit Magnifier). A key press inside the chord
// keeps the Win tap from opening the Start menu. Injected events carry LLKHF_INJECTED, which our
// own keyboard hook skips in magnify mode.
void InjectWinChord(WORD vk) {
    INPUT in[4] = {};
    for (auto& i : in) i.type = INPUT_KEYBOARD;
    in[0].ki.wVk = VK_LWIN;
    in[1].ki.wVk = vk;
    in[2].ki.wVk = vk;      in[2].ki.dwFlags = KEYEVENTF_KEYUP;
    in[3].ki.wVk = VK_LWIN; in[3].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(4, in, sizeof(INPUT));
}

// One Ctrl+Alt+wheel notch, modifiers held only for the microseconds around the wheel event so
// they can never leak onto the user's own concurrent clicks/keys. Injected events are skipped by
// our keyboard hook in magnify mode (setIgnoreInjectedKeys), and the mouse hook ignores wheels.
void InjectZoomNotch(bool zoomIn) {
    INPUT in[5] = {};
    in[0].type = INPUT_KEYBOARD; in[0].ki.wVk = VK_CONTROL;
    in[1].type = INPUT_KEYBOARD; in[1].ki.wVk = VK_MENU;
    in[2].type = INPUT_MOUSE;    in[2].mi.dwFlags = MOUSEEVENTF_WHEEL;
    in[2].mi.mouseData = (DWORD)(zoomIn ? WHEEL_DELTA : -WHEEL_DELTA);
    in[3].type = INPUT_KEYBOARD; in[3].ki.wVk = VK_MENU;    in[3].ki.dwFlags = KEYEVENTF_KEYUP;
    in[4].type = INPUT_KEYBOARD; in[4].ki.wVk = VK_CONTROL; in[4].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(5, in, sizeof(INPUT));
}

} // namespace

void MagnifyModel::launchMagnifier() {
    unsigned long long now = GetTickCount64();
    if (now - lastLaunchMs_ < 2000) return;          // backoff: launch takes a moment to appear
    lastLaunchMs_ = now;
    WriteMagDword(L"Magnification", 100);            // never launch into a leftover zoom level
    HINSTANCE h = ShellExecuteW(nullptr, L"open", L"magnify.exe", nullptr, nullptr, SW_SHOWMINNOACTIVE);
    wind::Log(wind::LogLevel::Info, "magnify", "launch magnify.exe -> %s",
              ((INT_PTR)h > 32) ? "ok" : "FAILED");
}

bool MagnifyModel::initialize(const MonitorTarget&) {
    backupPath_ = ResolveBackupPath();
    // One-shot snapshot of the user's Magnifier settings, BEFORE we modify anything. If the file
    // already exists we keep it: a previous Wind crashed before restoring, and re-snapshotting now
    // would capture OUR values as the user's.
    if (!backupPath_.empty() && GetFileAttributesW(backupPath_.c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::wofstream f(backupPath_.c_str());
        for (const wchar_t* name : kSnapshotValues)
            f << name << L"=" << ReadMagDword(name, -1) << L"\n";   // -1 = value was absent
    }
    WriteMagDword(L"MagnificationMode", 2);              // fullscreen (read at Magnifier startup)
    WriteMagDword(L"MagnifierUIWindowMinimized", 1);     // keep the toolbar out of the way
    launchMagnifier();
    ready_ = true;
    wind::Log(wind::LogLevel::Info, "magnify", "initialized (native wheel-notch drive)");
    return true;
}

void MagnifyModel::nativeZoomTick(int dir) {
    if (dir != lastDir_) {
        wind::Log(wind::LogLevel::Info, "magnify", "zoom %s",
                  dir > 0 ? "in (held)" : dir < 0 ? "out (held)" : "released");
        lastDir_ = dir;
    }
    if (dir == 0) return;
    unsigned long long now = GetTickCount64();
    if (now - lastNotchMs_ < kNotchIntervalMs) return;
    if (!MagnifierWindowPresent()) { launchMagnifier(); return; }   // user closed it: bring it back
    lastNotchMs_ = now;
    InjectZoomNotch(dir > 0);
}

void MagnifyModel::shutdown() {
    if (!ready_) return;
    if (MagnifierWindowPresent()) InjectWinChord(VK_ESCAPE);
    // Put the user's Magnifier settings back exactly as we found them (absent values were
    // snapshotted as -1: skip them rather than inventing a value).
    if (!backupPath_.empty()) {
        std::wifstream f(backupPath_.c_str());
        std::wstring line;
        while (std::getline(f, line)) {
            size_t eq = line.find(L'=');
            if (eq == std::wstring::npos) continue;
            std::wstring name = line.substr(0, eq);
            int value = _wtoi(line.substr(eq + 1).c_str());
            if (value >= 0) WriteMagDword(name.c_str(), value);
        }
        f.close();
        DeleteFileW(backupPath_.c_str());
        wind::Log(wind::LogLevel::Info, "magnify", "shutdown: Magnifier quit, registry restored");
    }
    ready_ = false;
}
}
