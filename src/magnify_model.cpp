#include "magnify_model.h"
#include "magnify_level.h"
#include "logging.h"
#include <windows.h>
#include <magnification.h>
#include <shellapi.h>
#include <cmath>
#include <cstdio>
#include <fstream>

namespace wind {
namespace {

const wchar_t* kMagKey = L"Software\\Microsoft\\ScreenMagnifier";
// The values we modify (and therefore snapshot + restore). Magnification is the live handoff
// channel (Magnifier registry-watches it); the rest are read at Magnifier's startup.
const wchar_t* kSnapshotValues[] = { L"Magnification", L"MagnificationMode",
                                     L"FollowMouse", L"MagnifierUIWindowMinimized" };

// A single-tick level jump this big is a SNAP (quick zoom), not a ramp tick: route it through
// the registry so Magnifier's eased animation plays it, instead of hard-setting the transform.
const double kSnapJump = 0.75;

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

// Current actual DWM fullscreen transform (level + source origin). Falls back to identity.
void ReadTransform(double& lvl, double& ox, double& oy) {
    float l = 1.0f; int x = 0, y = 0;
    if (MagGetFullscreenTransform(&l, &x, &y)) { lvl = l; ox = x; oy = y; }
    else { lvl = 1.0; ox = 0.0; oy = 0.0; }
}

} // namespace

bool MagnifyModel::magnifierRunning() const { return MagnifierWindowPresent(); }

void MagnifyModel::writeRegistryPct(int pct) {
    if (pct == lastRegPct_) return;   // same-value writes fire no notification anyway
    WriteMagDword(L"Magnification", pct);
    lastRegPct_ = pct;
}

void MagnifyModel::launchMagnifier() {
    unsigned long long now = GetTickCount64();
    if (now - lastLaunchMs_ < 2000) return;          // backoff: launch takes a moment to appear
    lastLaunchMs_ = now;
    HINSTANCE h = ShellExecuteW(nullptr, L"open", L"magnify.exe", nullptr, nullptr, SW_SHOWMINNOACTIVE);
    wind::Log(wind::LogLevel::Info, "magnify", "launch magnify.exe -> %s",
              ((INT_PTR)h > 32) ? "ok" : "FAILED");
}

bool MagnifyModel::initialize(const MonitorTarget&) {
    if (!MagInitialize()) {           // for MagGet/MagSetFullscreenTransform (the ramp channel)
        wind::Log(wind::LogLevel::Warn, "magnify", "MagInitialize failed");
        return false;
    }
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
    WriteMagDword(L"FollowMouse", 1);
    WriteMagDword(L"MagnifierUIWindowMinimized", 1);     // keep the toolbar out of the way
    WriteMagDword(L"Magnification", 100);
    lastRegPct_ = 100;
    // If the user's own Magnifier session is running it has stale startup-read settings; quit it
    // and start ours. Launching NOW (not lazily at first zoom-in) means Magnify.exe can never
    // initialize mid-ramp - unknown whether its startup would reset the transform we are driving.
    if (magnifierRunning()) { InjectWinChord(VK_ESCAPE); Sleep(150); }
    launchMagnifier();
    ready_ = true;
    wind::Log(wind::LogLevel::Info, "magnify", "initialized (hybrid transform+registry drive)");
    return true;
}

void MagnifyModel::present(const MapResult&, double level, const Config&,
                           const MonitorTarget& mon, const PresentExtras&) {
    double lvl = MagnifyClampLevel(level);
    double jump = std::fabs(lvl - lastLevel_);
    lastLevel_ = lvl;
    if (jump <= 1e-4) {
        // Level is settled. Hand off to Magnifier once: snap the transform to the exact integer
        // percent (so the registry value matches the actual transform bit-for-bit and the write
        // is a visual no-op), then write it. From here Magnifier's own panning drives the view.
        if (!synced_) {
            int pct = MagnifyTargetPct(lvl);
            double cur, ox, oy; ReadTransform(cur, ox, oy);
            POINT c; GetCursorPos(&c);
            MagnifyOffset off = MagnifyAnchorOffset(c.x, c.y, cur, ox, oy, pct / 100.0, mon.w, mon.h);
            MagSetFullscreenTransform((float)(pct / 100.0),
                                      (int)std::lround(off.x), (int)std::lround(off.y));
            writeRegistryPct(pct);
            synced_ = true;
        }
        if (!magnifierRunning()) launchMagnifier();   // user closed it manually: bring it back
        return;
    }
    synced_ = false;
    if (jump >= kSnapJump && MagnifyTargetPct(lvl) != lastRegPct_) {
        // Quick-zoom snap: let Magnifier's eased animation play it (measured: one registry write
        // eases from the ACTUAL current transform to the target over ~280 ms, any distance).
        writeRegistryPct(MagnifyTargetPct(lvl));
        synced_ = true;   // the registry route both moves the view and syncs Magnifier
        return;
    }
    // Active ramp tick: drive the transform directly - glass smooth, anchored at the cursor so
    // the view zooms toward/away from the point the user is looking at (Magnifier's own idle
    // writes are outpaced by this 144 Hz re-assert if they ever collide).
    double cur, ox, oy; ReadTransform(cur, ox, oy);
    POINT c; GetCursorPos(&c);
    MagnifyOffset off = MagnifyAnchorOffset(c.x, c.y, cur, ox, oy, lvl, mon.w, mon.h);
    MagSetFullscreenTransform((float)lvl, (int)std::lround(off.x), (int)std::lround(off.y));
}

void MagnifyModel::setActive(bool active) {
    if (active) return;
    // Zoom-out to idle. A hold-to-zoom ramp already walked the transform down through present(),
    // so the residual is tiny: snap it to identity. A quick-zoom-out arrives here with the
    // transform still high: route through the registry so Magnifier eases down (unless the
    // registry already says 100 - a same-value write fires no notification - then snap).
    double cur, ox, oy; ReadTransform(cur, ox, oy);
    if (cur - 1.0 >= kSnapJump && lastRegPct_ != 100) {
        writeRegistryPct(100);
    } else {
        MagSetFullscreenTransform(1.0f, 0, 0);
        writeRegistryPct(100);
    }
    lastLevel_ = 1.0;
    synced_ = true;
}

void MagnifyModel::shutdown() {
    if (!ready_) return;
    MagSetFullscreenTransform(1.0f, 0, 0);   // never leave the desktop zoomed
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
    MagUninitialize();
    ready_ = false;
}
}
