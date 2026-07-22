#include "magnify_model.h"
#include "magnify_level.h"
#include "logging.h"
#include <windows.h>
#include <magnification.h>
#include <shellapi.h>
#include <atomic>
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

// PUBLIC MagSetFullscreenTransform ONLY. The private SetMagnificationDesktopMagnification
// export (what the Settings zoom slider drives) was tried for ramps and made everything WORSE:
// display lag that ACCUMULATED across zooms (taps kept zooming long after release, escalating
// with use) - consistent with each call starting a DWM-side eased animation, so calls faster
// than the animation stack an ever-growing backlog. The public API applies immediately: the
// old transform model shipped it per-tick at composite pace for months, and the first hybrid
// build using it was the one whose zoom-in was judged "almost perfect". Do not reintroduce the
// private channel for per-tick streaming.
void SetTransform(double lvl, double ox, double oy) {
    MagSetFullscreenTransform((float)lvl, (int)std::lround(ox), (int)std::lround(oy));
}

typedef LONG (NTAPI* PFN_NtProcOp)(HANDLE);
PFN_NtProcOp NtSuspendFn() {
    static auto p = (PFN_NtProcOp)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtSuspendProcess");
    return p;
}
PFN_NtProcOp NtResumeFn() {
    static auto p = (PFN_NtProcOp)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtResumeProcess");
    return p;
}

// The process handle currently holding Magnify.exe suspended (null when none). Global so the
// crash filter / atexit net can resume without a model instance.
std::atomic<void*> g_suspendedMagnify{ nullptr };

} // namespace

void MagnifyEmergencyResume() {
    if (void* h = g_suspendedMagnify.exchange(nullptr)) {
        if (auto res = NtResumeFn()) res((HANDLE)h);
    }
}

bool MagnifyModel::magnifierRunning() const { return MagnifierWindowPresent(); }

void MagnifyModel::suspendMagnifier() {
    if (suspended_) return;
    HWND w = FindWindowW(L"MagUIClass", nullptr);
    if (!w) return;                                  // not running: nothing to silence
    DWORD pid = 0; GetWindowThreadProcessId(w, &pid);
    if (pid != pid_ || !hProc_) {                    // (re)acquire: Magnifier may have restarted
        if (hProc_) CloseHandle((HANDLE)hProc_);
        hProc_ = OpenProcess(PROCESS_SUSPEND_RESUME, FALSE, pid);
        pid_ = pid;
    }
    auto sus = NtSuspendFn();
    if (hProc_ && sus && sus((HANDLE)hProc_) >= 0) {
        suspended_ = true;
        g_suspendedMagnify.store(hProc_, std::memory_order_relaxed);
    } else if (!suspendWarned_) {
        // Dev (non-UIAccess) build: opening a UIAccess process is denied. Degrade to
        // re-assert-only ramps (a foreign write can flash for at most one tick).
        suspendWarned_ = true;
        wind::Log(wind::LogLevel::Warn, "magnify",
                  "cannot suspend Magnify.exe (dev build?); ramps run re-assert-only");
    }
}

void MagnifyModel::resumeMagnifier() {
    if (!suspended_) return;
    suspended_ = false;
    g_suspendedMagnify.store(nullptr, std::memory_order_relaxed);
    if (auto res = NtResumeFn()) res((HANDLE)hProc_);
}

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
    // Ramp detection uses the UNCLAMPED level: a hold past the 16x ceiling must keep reading as
    // "ramping" (clamped jump would read 0 and fire a premature mid-hold settle/handoff).
    double jump = std::fabs(level - lastLevel_);
    lastLevel_ = level;
    int pct = MagnifyTargetPct(lvl);
    if (jump <= 1e-4) {
        // Level is settled.
        if (phase_ == Phase::Ramping) {
            // Freeze the view on the exact integer percent (so the later registry value matches
            // the actual transform bit-for-bit) and WAKE Magnifier. Do NOT write the registry
            // yet: Magnifier's notification handler animates from its internally OBSERVED
            // transform, and that observer was suspended with us mid-ramp - writing now would
            // replay an animation from the stale pre-ramp level (measured, probe 5). Give the
            // observer a few frames to see the settled transform first.
            POINT c; GetCursorPos(&c);
            MagnifyOffset off = MagnifyAnchorOffset(c.x, c.y, curLvl_, curOx_, curOy_,
                                                    pct / 100.0, mon.w, mon.h);
            curLvl_ = pct / 100.0; curOx_ = off.x; curOy_ = off.y;
            SetTransform(curLvl_, curOx_, curOy_);
            resumeMagnifier();
            wind::Log(wind::LogLevel::Info, "magnify",
                      "ramp end: %.2f -> %.2f, %d sets in %llums; handoff to pct=%d",
                      rampStartLvl_, curLvl_, rampSets_,
                      GetTickCount64() - rampStartMs_, pct);
            phase_ = Phase::Handoff;
            handoffTicks_ = 12;  // ~84 ms at 144 Hz: 6 ticks observer catch-up, write, 6 ticks guard
                                 //   (>= 30 ms measured safe after resume; margin is free here)
        } else if (phase_ == Phase::Handoff) {
            // Guard window around the silent sync: keep re-asserting the settled transform so a
            // stale write from the just-woken Magnifier (e.g. a mouse-move handler firing before
            // it processed the sync) can flash for at most one tick.
            SetTransform(curLvl_, curOx_, curOy_);
            --handoffTicks_;
            if (handoffTicks_ == 6) writeRegistryPct(pct);   // observer now sees the settled level: no-op sync
            if (handoffTicks_ <= 0) phase_ = Phase::Idle;    // Magnifier owns steady state (panning)
        } else {
            if (!magnifierRunning()) launchMagnifier();   // user closed it manually: bring it back
        }
        return;
    }
    if (jump >= kSnapJump && pct != lastRegPct_) {
        // Quick-zoom snap: let Magnifier's eased animation play it (measured: one registry write
        // eases from the observed current transform to the target over ~280 ms, any distance).
        // If this interrupts a suspended ramp, the observer is STALE (frozen at the pre-ramp
        // level); writing immediately would replay an ease from there (measured, probe 5 T4b).
        // Give it the measured-safe catch-up pause first - a one-frame hitch on a snap is fine.
        bool wasSuspended = suspended_;
        resumeMagnifier();
        if (wasSuspended) Sleep(60);
        writeRegistryPct(pct);
        wind::Log(wind::LogLevel::Info, "magnify", "snap route: jump=%.2f -> pct=%d (wasSuspended=%d)",
                  jump, pct, (int)wasSuspended);
        phase_ = Phase::Idle;   // the registry route both moves the view and syncs Magnifier
        return;
    }
    // Active ramp tick: drive the transform directly - glass smooth, anchored at the cursor so
    // the view zooms toward/away from the point the user is looking at. Segment start reads the
    // actual transform ONCE (wherever Magnifier's panning left it) and SUSPENDS Magnify.exe: it
    // cannot write a single thing while we ramp, whatever its triggers are. Registry writes are
    // strictly forbidden in this phase - ANY value change on the ScreenMagnifier key (even
    // FollowMouse) wakes Magnifier's watcher, which re-applies the stale registry level mid-ramp
    // (measured: the constant stage-flicker + random release levels of the FollowMouse attempt).
    if (phase_ != Phase::Ramping) {
        ReadTransform(curLvl_, curOx_, curOy_);
        suspendMagnifier();
        phase_ = Phase::Ramping;
        rampSets_ = 0; rampStartLvl_ = curLvl_; rampStartMs_ = GetTickCount64();
        wind::Log(wind::LogLevel::Info, "magnify", "ramp start: from %.2f (suspended=%d)",
                  curLvl_, (int)suspended_);
    }
    POINT c; GetCursorPos(&c);
    MagnifyOffset off = MagnifyAnchorOffset(c.x, c.y, curLvl_, curOx_, curOy_, lvl, mon.w, mon.h);
    SetTransform(lvl, off.x, off.y);
    curLvl_ = lvl; curOx_ = off.x; curOy_ = off.y;
    ++rampSets_;
}

void MagnifyModel::setActive(bool active) {
    if (active) return;
    // Zoom-out to idle. Wake Magnifier first (registry routes need its watcher alive). A
    // hold-to-zoom ramp already walked the transform down through present(), so the residual is
    // tiny: snap it to identity. A quick-zoom-out arrives here with the transform still high:
    // route through the registry so Magnifier eases down (unless the registry already says 100 -
    // a same-value write fires no notification - then snap).
    bool wasSuspended = suspended_;
    resumeMagnifier();
    // THE zoom-out bug (measured): after a suspended zoom-out ramp, Magnifier's observer is
    // frozen at the level the ramp STARTED from; a registry write before it catches up replays
    // an ease from that stale high level - the view zooms back IN and out again. >= 30 ms after
    // resume is measured clean; pause 60 ms (idle transition, imperceptible) before any write.
    if (wasSuspended) Sleep(60);
    double cur, ox, oy; ReadTransform(cur, ox, oy);
    bool easeRoute = cur - 1.0 >= kSnapJump && lastRegPct_ != 100;
    if (easeRoute) {
        writeRegistryPct(100);
    } else {
        SetTransform(1.0, 0, 0);
        writeRegistryPct(100);
    }
    wind::Log(wind::LogLevel::Info, "magnify", "idle: actual=%.2f route=%s (wasSuspended=%d)",
              cur, easeRoute ? "registry-ease" : "snap", (int)wasSuspended);
    lastLevel_ = 1.0;
    phase_ = Phase::Idle;
}

void MagnifyModel::shutdown() {
    if (!ready_) return;
    resumeMagnifier();                       // never leave Magnify.exe suspended
    SetTransform(1.0, 0.0, 0.0);             // never leave the desktop zoomed
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
    if (hProc_) { CloseHandle((HANDLE)hProc_); hProc_ = nullptr; pid_ = 0; }
    MagUninitialize();
    ready_ = false;
}
}
