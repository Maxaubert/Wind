#include "magnify_model.h"
#include "magnify_level.h"
#include "logging.h"
#include <windows.h>
#include <shellapi.h>
#include <cstdio>
#include <fstream>

namespace wind {
namespace {

const wchar_t* kMagKey = L"Software\\Microsoft\\ScreenMagnifier";
// The values we modify (and therefore snapshot + restore). Magnification is the LIVE zoom
// channel (Magnifier registry-watches it); the rest are read at Magnifier's startup.
const wchar_t* kSnapshotValues[] = { L"Magnification", L"MagnificationMode",
                                     L"FollowMouse", L"MagnifierUIWindowMinimized" };

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

} // namespace

bool MagnifyModel::magnifierRunning() const { return MagnifierWindowPresent(); }

void MagnifyModel::launchMagnifier() {
    unsigned long long now = GetTickCount64();
    if (now - lastLaunchMs_ < 2000) return;          // backoff: launch takes a moment to appear
    lastLaunchMs_ = now;
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
    // Startup-read values first; the live Magnification channel starts at 1x. If Magnifier is
    // already running (the user's own session), quit it so the next launch picks these up.
    WriteMagDword(L"MagnificationMode", 2);              // fullscreen
    WriteMagDword(L"FollowMouse", 1);
    WriteMagDword(L"MagnifierUIWindowMinimized", 1);     // keep the toolbar out of the way
    WriteMagDword(L"Magnification", 100);
    lastWrittenPct_ = 100;
    if (magnifierRunning()) InjectWinChord(VK_ESCAPE);
    ready_ = true;
    wind::Log(wind::LogLevel::Info, "magnify", "initialized (live registry drive)");
    return true;
}

void MagnifyModel::present(const MapResult&, double level, const Config&,
                           const MonitorTarget&, const PresentExtras&) {
    // The whole job: keep the registry Magnification equal to the ramped level's integer percent.
    // Magnifier registry-watches the value and eases to it (~100 ms trailing), so the zoom runs
    // at Wind's configured speed and stops when the user releases - no backlog, no steps.
    int pct = MagnifyTargetPct(level);
    if (pct != lastWrittenPct_) {
        WriteMagDword(L"Magnification", pct);
        lastWrittenPct_ = pct;
    }
    // Lazy-launch on the first real zoom-in (and relaunch if the user closed Magnifier manually);
    // it starts directly at the current pct since the registry is already written.
    if (pct > 100 && !magnifierRunning()) launchMagnifier();
}

void MagnifyModel::setActive(bool active) {
    if (active) return;
    // Zoom-out to idle: park the level at 1x and KEEP Magnifier running (user decision) - at 100%
    // the fullscreen transform is identity (zero visual effect) and the next zoom-in is instant.
    if (lastWrittenPct_ != 100) {
        WriteMagDword(L"Magnification", 100);
        lastWrittenPct_ = 100;
    }
}

void MagnifyModel::shutdown() {
    if (!ready_) return;
    if (magnifierRunning()) InjectWinChord(VK_ESCAPE);
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
