#include "magnify_model.h"
#include "magnify_steps.h"
#include "logging.h"
#include <windows.h>
#include <shellapi.h>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace wind {
namespace {

const wchar_t* kMagKey = L"Software\\Microsoft\\ScreenMagnifier";
// The values we modify (and therefore snapshot + restore). Magnifier reads them at ITS startup;
// the live control channel is the Win+Plus/Win+Minus hotkeys it registers globally.
const wchar_t* kSnapshotValues[] = { L"Magnification", L"ZoomIncrement", L"MagnificationMode",
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

// Inject a Win+<vk> chord. A key press inside the chord keeps the Win tap from opening the Start
// menu. Injected events carry LLKHF_INJECTED, which our own keyboard hook skips in magnify mode.
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

int MagnifyModel::readMagnificationPct() const {
    return ReadMagDword(L"Magnification", -1);
}

bool MagnifyModel::magnifierRunning() const { return MagnifierWindowPresent(); }

void MagnifyModel::launchMagnifier() {
    unsigned long long now = GetTickCount64();
    if (now - lastLaunchMs_ < 2000) return;          // backoff: launch takes a moment to appear
    lastLaunchMs_ = now;
    // Start at 1x, always: Magnifier restores its last level from the registry, and a leftover
    // 500% from a previous session would slam the screen on launch.
    WriteMagDword(L"Magnification", 100);
    currentSteps_ = 0;
    HINSTANCE h = ShellExecuteW(nullptr, L"open", L"magnify.exe", nullptr, nullptr, SW_SHOWMINNOACTIVE);
    launched_ = ((INT_PTR)h > 32);
    wind::Log(wind::LogLevel::Info, "magnify", "launch magnify.exe -> %s",
              launched_ ? "ok" : "FAILED");
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
    // Magnifier reads these at startup. If it is already running (the user's own session), quit it
    // so the next launch (ours) picks them up.
    WriteMagDword(L"MagnificationMode", 2);              // fullscreen
    WriteMagDword(L"ZoomIncrement", stepPct_);           // small steps = the near-smooth ramp
    WriteMagDword(L"FollowMouse", 1);
    WriteMagDword(L"MagnifierUIWindowMinimized", 1);     // keep the toolbar out of the way
    WriteMagDword(L"Magnification", 100);
    if (magnifierRunning()) InjectWinChord(VK_ESCAPE);
    ready_ = true;
    wind::Log(wind::LogLevel::Info, "magnify", "initialized (step=%d%%)", stepPct_);
    return true;
}

void MagnifyModel::injectZoomChord(bool zoomIn) {
    InjectWinChord(zoomIn ? VK_OEM_PLUS : VK_OEM_MINUS);
}

void MagnifyModel::syncToTarget(int targetSteps, int budget) {
    int n = MagnifyInjectionsThisTick(currentSteps_, targetSteps, budget);
    for (int i = 0; i < (n > 0 ? n : -n); ++i) {
        injectZoomChord(n > 0);
        currentSteps_ += (n > 0) ? 1 : -1;
        if (i + 1 < (n > 0 ? n : -n)) Sleep(5);   // space chords so Magnifier never drops one
    }
}

void MagnifyModel::present(const MapResult&, double level, const Config&,
                           const MonitorTarget&, const PresentExtras&) {
    int target = MagnifyTargetSteps(level, stepPct_);
    if (!magnifierRunning()) {
        if (target > 0) launchMagnifier();   // lazy: first zoom-in starts Magnifier (at 100%)
        return;                              // injections begin once its window exists
    }
    if (target != currentSteps_) {
        syncToTarget(target, 3);             // ~3 steps/tick tracks the ramp without flooding
        lastResyncMs_ = GetTickCount64();    // don't resync against a mid-ramp registry value
        return;
    }
    // At target and idle: adopt Magnifier's own idea of the level if it disagrees (the user may
    // have pressed Win+Plus/Minus themselves; Magnifier persists the value as it changes).
    unsigned long long now = GetTickCount64();
    if (now - lastResyncMs_ >= 1000) {
        lastResyncMs_ = now;
        int pct = readMagnificationPct();
        if (pct >= 100) {
            int steps = MagnifyTargetSteps(pct / 100.0, stepPct_);
            if (steps != currentSteps_) {
                wind::Log(wind::LogLevel::Info, "magnify", "resync: registry %d%% -> %d steps (was %d)",
                          pct, steps, currentSteps_);
                currentSteps_ = steps;
            }
        }
    }
}

void MagnifyModel::setActive(bool active) {
    if (active || !magnifierRunning()) return;
    // Zoom-out to idle. The per-tick sync already walked most of the way down during the ramp; a
    // small residual (final partial tick) is burst down. A LARGE residual (quick-zoom snap from a
    // high level) would mean dozens of spaced chords, so quit + relaunch at 100% instead - the
    // user chose keep-Magnifier-running-at-idle, and a backgrounded relaunch honors that with a
    // deterministic level reset.
    if (currentSteps_ != 0 && currentSteps_ <= 10) {
        syncToTarget(0, currentSteps_);
    } else if (currentSteps_ != 0) {
        InjectWinChord(VK_ESCAPE);
        Sleep(150);                          // let Magnifier process the quit before relaunching
        lastLaunchMs_ = 0;                   // bypass the backoff: this relaunch is intentional
        launchMagnifier();
    }
    currentSteps_ = 0;
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
