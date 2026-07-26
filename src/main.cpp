#include <windows.h>
#include <dwmapi.h>
#include <tlhelp32.h>
#include <magnification.h>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstdarg>
#include <memory>
#include <set>
#include <sstream>
#include <fstream>
#include "config.h"
#include "config_path.h"
#include "config_ui/ini_edit.h"   // wind::UpdateIniText - flip the model key in place
#include "logging.h"
#pragma comment(lib, "Dwmapi.lib")
#include "render_engine.h"
#include "render_model.h"
#include "magnify_model.h"
#include "transform_model.h"
#include "input_router.h"
#include "cursor_mapper.h"
#include "zoom_controller.h"
#include "tray.h"
#include "lock_detector.h"
#include "cursor_lock.h"
#include "inspect_focus.h"
#include "resource.h"

using namespace wind;

static InputRouter g_input;

// Issue #148 ROOT CAUSE (proven 2026-07-26 via the MPO-off experiment): NVIDIA's multiplane-
// overlay (MPO) plane programming packs DWM's magnification translation into a 16-bit field.
// With a game surface on a hardware plane, |srcX*level| > 32767 (= the far-right strip above
// ~9.3x on 3840) wraps and resets the driver (nvlddmkm 153). With MPO disabled
// (HKLM\...\Dwm\OverlayTestMode=5) the identical writes are clean at full range - DWM
// composites in float. The pan wall below therefore applies ONLY while MPO is enabled.
static constexpr double kMaxSafeTxMagnitude = 32000.0;
static bool g_mpoDisabled = false;   // boot state of OverlayTestMode (read once at startup)

static void DetectMpoDisabled() {
    DWORD v = 0, sz = sizeof(v);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\Dwm",
                     L"OverlayTestMode", RRF_RT_REG_DWORD, nullptr, &v, &sz) == ERROR_SUCCESS)
        g_mpoDisabled = (v == 5);
    wind::Log(wind::LogLevel::Info, "startup", "MPO %s (OverlayTestMode=%lu) -> pan wall %s",
              g_mpoDisabled ? "DISABLED" : "enabled", (unsigned long)v,
              g_mpoDisabled ? "off (full range)" : "on (right-strip bound above ~9.3x)");
}

// Current refresh rate (Hz) of the primary display, for pacing the idle/1x loop and the
// vsync=0 path so we don't hardcode the dev's 144Hz. Falls back to 60 if the query fails or
// reports a placeholder (some drivers report 0/1 for "hardware default").
// Refresh rate of a specific display (GDI device name, e.g. "\\.\DISPLAY2"); nullptr/empty = the
// primary/current display. Re-queried on retarget so pacing tracks the monitor we're actually on
// (a mixed-refresh multi-monitor setup would otherwise pace a 60Hz panel at the startup 144) (#74).
static int DetectRefreshHz(const wchar_t* device = nullptr) {
    DEVMODEW dm{}; dm.dmSize = sizeof(dm);
    const wchar_t* dev = (device && device[0]) ? device : nullptr;
    if (EnumDisplaySettingsW(dev, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)
        return (int)dm.dmDisplayFrequency;
    return 60;
}

// The primary monitor as a MonitorTarget (origin 0,0, primary size, empty device name = first
// DXGI output). This is the legacy single-monitor target and the universal fallback.
static MonitorTarget PrimaryMonitor() {
    MonitorTarget t;
    t.x = 0; t.y = 0;
    t.w = GetSystemMetrics(SM_CXSCREEN);
    t.h = GetSystemMetrics(SM_CYSCREEN);
    t.device[0] = L'\0';
    return t;
}

// The monitor the cursor is currently on, as a MonitorTarget. Falls back to the primary if the
// query fails. Used at startup and on each zoom-in (when multiMonitor is on).
static MonitorTarget MonitorUnderCursor() {
    POINT pt; GetCursorPos(&pt);
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFOEXW mi{}; mi.cbSize = sizeof(mi);
    if (mon && GetMonitorInfoW(mon, &mi)) {
        MonitorTarget t;
        t.x = mi.rcMonitor.left;
        t.y = mi.rcMonitor.top;
        t.w = mi.rcMonitor.right - mi.rcMonitor.left;
        t.h = mi.rcMonitor.bottom - mi.rcMonitor.top;
        lstrcpynW(t.device, mi.szDevice, 32);
        return t;
    }
    return PrimaryMonitor();
}

// Whether two targets are the same monitor (origin + size + device name).
static bool SameMonitor(const MonitorTarget& a, const MonitorTarget& b) {
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h && wcscmp(a.device, b.device) == 0;
}

// True when the foreground window covers the whole target monitor - i.e. a fullscreen / borderless
// app (typically a game). Such an app is usually promoted to an independent-flip / MPO plane that
// Desktop Duplication can't see until our overlay forces DWM to composite it, which is what makes
// the first zoom-in flash the previously-focused window (issue #90). We use the bridged reveal only
// in this case so ordinary desktop zoom-ins keep the instant path.
static bool ForegroundCoversMonitor(const MonitorTarget& mon) {
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    RECT wr{};
    if (!GetWindowRect(fg, &wr)) return false;
    return wr.left <= mon.x && wr.top <= mon.y &&
           wr.right >= mon.x + mon.w && wr.bottom >= mon.y + mon.h;
}

// --- Game-inspect focus steal (issue #144) ---------------------------------------------------
// A mouselook game reads the mouse via Raw Input, which no user-mode hook can block from another
// process (the documented LL-hook limitation): under Inspect the camera kept turning and the game
// fought the 1px freeze clip by recentering every frame. Backgrounding the game is the one
// user-mode lever that works - games register raw input without RIDEV_INPUTSINK and DirectInput's
// foreground cooperative level drops too, so on focus loss the camera freezes and the game
// releases its clip (exactly why Snipping Tool's overlay works over gameplay). Foreground goes to
// this invisible 1x1 helper (layered alpha 0: never painted, never seen), NOT the overlay - the
// overlay must stay WS_EX_NOACTIVATE + click-through. Wind's own pan is unaffected: raw input is
// registered with RIDEV_INPUTSINK on a message-only window and arrives regardless of foreground.
static HWND g_focusStealer = nullptr;
static HWND EnsureFocusStealer(const MonitorTarget& mon) {
    if (g_focusStealer && IsWindow(g_focusStealer)) {
        SetWindowPos(g_focusStealer, nullptr, mon.x, mon.y, 1, 1,
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        return g_focusStealer;
    }
    static ATOM s_atom = 0;
    if (!s_atom) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"WindFocusStealer";
        s_atom = RegisterClassW(&wc);
    }
    if (!s_atom) return nullptr;
    // No WS_EX_NOACTIVATE (it exists to accept activation); TOOLWINDOW keeps it out of the
    // taskbar and alt-tab list.
    g_focusStealer = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
                                     L"WindFocusStealer", L"Wind Inspect", WS_POPUP,
                                     mon.x, mon.y, 1, 1, nullptr, nullptr,
                                     GetModuleHandleW(nullptr), nullptr);
    if (!g_focusStealer) return nullptr;
    SetLayeredWindowAttributes(g_focusStealer, 0, 0, LWA_ALPHA);   // fully invisible
    ShowWindow(g_focusStealer, SW_SHOWNOACTIVATE);   // shown (a hidden window can't take foreground)
    return g_focusStealer;
}

// SetForegroundWindow can silently refuse under the foreground-lock rules (the signed UIAccess
// build is exempt). Verify the result, then fall back to the AttachThreadInput handshake so the
// unsigned dev build works too.
static bool StealForeground(HWND w) {
    if (!w) return false;
    if (GetForegroundWindow() == w) return true;
    SetForegroundWindow(w);
    if (GetForegroundWindow() == w) return true;
    HWND fg = GetForegroundWindow();
    DWORD fgTid = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    DWORD myTid = GetCurrentThreadId();
    if (fgTid && fgTid != myTid && AttachThreadInput(myTid, fgTid, TRUE)) {
        SetForegroundWindow(w);
        AttachThreadInput(myTid, fgTid, FALSE);
    }
    return GetForegroundWindow() == w;
}

// Virtual-desktop bounds (the union of all monitors), used per zoomed frame to detect a game
// clipping the cursor. Cached because GetSystemMetrics is a syscall and these bounds change only
// on a display-topology change; refreshed on each zoom-in (where we also retarget the monitor).
struct VirtualBounds { int x, y, w, h; };
static VirtualBounds QueryVirtualBounds() {
    return { GetSystemMetrics(SM_XVIRTUALSCREEN),  GetSystemMetrics(SM_YVIRTUALSCREEN),
             GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN) };
}

// --- Per-tick state -------------------------------------------------------------------------
// All the state one magnifier tick mutates, in one struct so the tick can run from BOTH the
// main loop AND a WM_TIMER. The tray context menu's TrackPopupMenu spins its own modal message
// loop that owns the thread until it closes; without a timer-driven tick the lens froze for
// the duration. The timer (set around the menu) dispatches WM_TIMER into WndProc, which ticks.
struct TickState {
    IMagnifierModel* model;                    // CURRENT engine (hybrid swaps at zoom-in)
    IMagnifierModel* mRender = nullptr;        // hybrid: the two engines (null when not hybrid)
    IMagnifierModel* mTransform = nullptr;
    MonitorTarget    mon;       // current target monitor (origin + size + device name)
    Config         cfg;
    ZoomController zoom;
    CursorMapper   mapper;
    LockDetector   detector;    // free vs game-locked cursor
    POINT          lastSetVirtual{};  // where we last SetCursorPos'd (virtual px); for the OS-cursor delta
    VirtualBounds  vbounds{};   // cached virtual-screen bounds; refreshed on zoom-in (used for clip detect)
    LARGE_INTEGER freq{}, prev{};
    double sinceCheck = 0.0;
    unsigned long long lastMtime = 0;
    HANDLE configWatch = nullptr;              // ini-dir change notification (replaces the 1Hz mtime poll)
    std::wstring iniPath;                      // full path to magnifier.ini, resolved at startup
    double prevLvl = 1.0;
    int    revealPending = 0;                  // fallback-cap ticks left on the gated reveal; the
                                               //   reveal itself is evidence-gated (#90, #140)
    IMagnifierModel* restAfterReveal = nullptr; // instant-switch handover: the OUTGOING engine
                                               //   stays live until the incoming one is on screen
                                               //   plus a small overlap (else a bare unmagnified
                                               //   composite flashes through the channel gap)
    int    restOverlapTicks = 0;               // overlap countdown once the handover condition met
    bool   revealNeedsComposite = false;       // fullscreen-app zoom-in: also require a post-prime
                                               //   composite in the capture before revealing
    int    hz = 60;                            // resolved tick/refresh rate (auto-detected)
    bool   recenterKeyWasDown = false;         // edge-detect the recenterVk key
    bool   swapKeyWasDown = false;             // edge-detect the swapModelVk key (model swap + restart)
    bool   swapArmed = false;                  // swap fires only after the key is first seen UP (a key
                                               //   held across a relaunch must not re-trigger a swap)
    CursorLockController cursorLock;            // Inspect mode (freeze-cursor + free-look reticle toggle)
    bool   lockKeyWasDown = false;             // edge-detect the cursorLockVk toggle
    bool   prevInspect = false;     // Inspect was on last tick (detect freeze enter/exit)
    bool   prevActive = false;      // overlay was active last tick (zoomed OR inspect)
    POINT  frozenCursor{};          // where the real cursor is frozen while Inspect is on (virtual px)
    int    clickReleaseTicks = 0;   // after a committed click: ticks to keep the freeze clip released so
                                    //   the synthesized click reaches the look point (then re-freeze)
    double inspectPanRemX = 0.0;    // sub-pixel carry for the cooked Inspect-mode pan (slow motion not lost)
    double inspectPanRemY = 0.0;
    bool   inspectGame = false;         // game-inspect (issue #144): foreground stolen from a mouselook
                                        //   game so its raw-input camera stops receiving the mouse
    bool   inspectStealPending = false; // steal deferred past the reveal logic (it must read the true fg)
    HWND   inspectPrevFg = nullptr;     // the game window foreground is handed back to on exit
    bool   gameFreeze = false;      // transform GAME session (issue #148 TDR): the cursor is frozen
                                    //   (1px clip) so NO cursor-position update - hand, game, or ours -
                                    //   can race a transform write in DWM/the driver (the proven crash)
    POINT  freezePoint{};           // where the cursor is frozen during a transform game session
    int    freezePauseTicks = 0;    // ticks to skip transform writes around an injected click move
    bool   freezeStealPending = false;   // tdrTest=3: steal foreground on the next freeze tick
    HWND   freezePrevFg = nullptr;       // tdrTest=3: window to hand foreground back to
    HCURSOR lastFgCursor = nullptr; // churn valve: last seen foreground cursor SHAPE handle
    int    churnCount = 0;          //   handle changes inside the rolling window
    unsigned long long churnWinStart = 0;
    std::wstring freezeExe;         // exe of the app under the current/last transform game session
    unsigned long long lastFreezeActiveMs = 0;   // TDR-backstop window (device-lost attribution)
    bool   inspectCursorWasShowing = true; // cursor visibility at the toggle edge (the 1x mouselook tell)
    double presentAccum       = 0.0;           // gameFpsCap: seconds since the last presented frame
    bool   gamePacing         = false;         // zoomed over a fullscreen game -> main loop timer
                                               //   paces (a blocking present must never pace: a
                                               //   saturated GPU can starve it and wedge the tick)
    int    pushPhase          = 0;             // reduced-push game mode: tick index within the
                                               //   present divisor (0 = present tick)
    double quickZoomStored    = 0.0;           // remembered quick-zoom level (0 = none yet); in-memory
    bool   prevInHeld         = false;         // for rising-edge detection of the zoom-in channel
    bool   prevOutHeld        = false;
    // Diagnostics (issue #113): held-state edge logging for the intermittent stuck side-button. Track
    // the previous-tick held flags + how long the current held episode has lasted, so we can log a
    // snapshot (with the hook/raw event counters) on each rise/fall and flag a hold that overstays.
    bool   dbgPrevInHeld      = false;
    bool   dbgPrevOutHeld     = false;
    double dbgInHeldSec       = 0.0;
    double dbgOutHeldSec      = 0.0;
    bool   dbgInOverstayLogged  = false;       // one overstay WARN per stuck episode
    bool   dbgOutOverstayLogged = false;
    std::atomic<bool> quickZoomHotkey{false};  // set by WM_HOTKEY (hotkey-mode quick zoom), consumed in RunTick
    bool   cursorHidden       = false;         // runtime-only override (no ini write, no hot-reload)
    double outlineIdleSec = 0.0;   // seconds the cursor has been still (drives the outline idle fade)
    double outlineZoneSec = 0.0;   // seconds continuously in the low-zoom band (drives the show dwell)
    HWND   hwnd               = nullptr;       // owning message window (for RegisterHotKey)
    // Frame-pacing diagnostics (diagnostics=1): a 2 s window of loop-interval stats.
    double diagAccum = 0.0, diagSumDt = 0.0, diagMaxDt = 0.0;
    int    diagFrames = 0, diagHitches = 0;
    TickState(IMagnifierModel* mdl, const MonitorTarget& m, const Config& c)
        : model(mdl), mon(m), cfg(c),
          zoom(1.0, c.maxLevel),
          mapper(m.w, m.h, c.cursorSmoothing) {}
};
static TickState* g_tick = nullptr;

// --- Churny-app registry (issue #148 trigger 3). Rig-proven: a fullscreen app that CHURNS its
// cursor SHAPE (SetCursor from hover logic - what real games do whenever the mouse moves) makes
// per-tick fullscreen-transform writes reset the GPU driver within seconds, at ANY write rate
// (50Hz still died; quiet-cursor apps are clean at 144Hz). Wind cannot stop another process's
// SetCursor traffic, so hybrid LEARNS: a transform game session that detects shape churn
// instant-switches to render and records the app here; later zoom-ins over it pick render
// directly. Persisted so the lesson survives restarts. The render device-lost path is the
// backstop: a TDR that slips through (e.g. an animated cursor, invisible to handle polling)
// marks the app too - one crash ever per exotic app, then never again.
static std::set<std::wstring> g_churnyApps;

static std::wstring ChurnyFilePath() {
    std::wstring dir = wind::ResolveLogDir();          // %LOCALAPPDATA%\Wind\logs
    size_t cut = dir.find_last_of(L"\\/");
    if (cut != std::wstring::npos) dir.resize(cut);    // -> %LOCALAPPDATA%\Wind
    return dir + L"\\churny_apps.txt";
}
static std::wstring ExeNameOf(HWND h) {
    DWORD pid = 0;
    if (!h) return L"";
    GetWindowThreadProcessId(h, &pid);
    if (!pid) return L"";
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return L"";
    wchar_t buf[MAX_PATH]; DWORD len = MAX_PATH;
    std::wstring name;
    if (QueryFullProcessImageNameW(p, 0, buf, &len)) {
        std::wstring full(buf, len);
        size_t cut = full.find_last_of(L"\\/");
        name = cut != std::wstring::npos ? full.substr(cut + 1) : full;
        for (auto& c : name) c = (wchar_t)towlower(c);
    }
    CloseHandle(p);
    return name;
}
static void LoadChurnyApps() {
    FILE* f = nullptr;
    if (_wfopen_s(&f, ChurnyFilePath().c_str(), L"rt, ccs=UTF-8") != 0 || !f) return;
    wchar_t line[MAX_PATH];
    while (fgetws(line, MAX_PATH, f)) {
        std::wstring s(line);
        while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r')) s.pop_back();
        if (!s.empty()) g_churnyApps.insert(s);
    }
    fclose(f);
}
static void MarkChurnyApp(const std::wstring& exe, const char* why) {
    if (exe.empty() || g_churnyApps.count(exe)) return;
    g_churnyApps.insert(exe);
    FILE* f = nullptr;
    if (_wfopen_s(&f, ChurnyFilePath().c_str(), L"at, ccs=UTF-8") == 0 && f) {
        fwprintf(f, L"%s\n", exe.c_str());
        fclose(f);
    }
    wind::Log(wind::LogLevel::Info, "churn", "app marked churny (%s): %S -> render for games",
              why, exe.c_str());
}
static bool IsChurnyFg(HWND fg) {
    if (g_churnyApps.empty()) return false;
    return g_churnyApps.count(ExeNameOf(fg)) != 0;
}

// tdrTest=3 (issue #148 harness): hand foreground back when a stolen-foreground freeze session
// ends. Only if we still hold it - an alt-tab to a third app is respected.
static void EndFreezeSteal(TickState& t) {
    t.freezeStealPending = false;
    if (t.freezePrevFg && IsWindow(t.freezePrevFg) &&
        g_focusStealer && GetForegroundWindow() == g_focusStealer) {
        SetForegroundWindow(t.freezePrevFg);
    }
    t.freezePrevFg = nullptr;
}

// Hand foreground back to the game when game-inspect ends. Called on EVERY inspect exit path
// (toggle-off, zoom-out teardown, device-lost recovery, shutdown), mirroring the ClipCursor
// release invariant. Foreground is returned only if we still hold it - a user who alt-tabbed to
// a third app mid-inspect keeps their choice.
static void EndGameInspect(TickState& t) {
    if (!t.inspectGame) return;
    t.inspectGame = false;
    t.inspectStealPending = false;
    if (t.inspectPrevFg && IsWindow(t.inspectPrevFg) &&
        g_focusStealer && GetForegroundWindow() == g_focusStealer) {
        SetForegroundWindow(t.inspectPrevFg);
    }
    t.inspectPrevFg = nullptr;
    wind::Log(wind::LogLevel::Info, "inspect", "game-inspect ended (foreground returned)");
}

// Append a line to %TEMP%\wind_diag.log (frame-pacing diagnostics; gated on diagnostics=1).
// %TEMP% so it works for the Program Files deploy too (its own dir isn't writable).
static void DiagLog(const char* fmt, ...) {
    char path[MAX_PATH]; DWORD n = GetTempPathA(MAX_PATH, path);
    if (n == 0 || n > MAX_PATH) return;
    lstrcatA(path, "wind_diag.log");
    FILE* f = nullptr; if (fopen_s(&f, path, "a") != 0 || !f) return;
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f); fclose(f);
}

// Forward-declared so RunTick can re-register the hide-cursor hotkey on config hot-reload;
// the definition (with the static state it manages) lives near WndProc / kHideCursorHotkeyId.
static void RegisterHideCursorHotkey(HWND hwnd, int vk, int mods);
// Same pattern for the quick-zoom hotkey (hotkey mode). Pass vk=0 to unregister.
static void RegisterQuickZoomHotkey(HWND hwnd, int vk, int mods);
// Forward-declared so RunTick can call it on the swapModelVk rising edge; the definition lives near
// the single-instance helpers below (it relaunches Wind.exe, which drives the same eviction handshake).
static void SwapModelAndRelaunch(const std::wstring& iniPath, const std::string& currentModel);

// Read the current Windows pointer-speed + acceleration settings into a BallisticsConfig so Inspect
// mode pans the look point at the same speed as the desktop cursor. Refreshed on each Inspect entry
// (these settings change rarely). SystemParametersInfo only: the SmoothMouse curve shape is the
// standard hardcoded default (rarely customized) and is normalized to the slider baseline in
// mouse_ballistics, so its absolute scale does not matter.
static BallisticsConfig ReadMouseBallistics() {
    BallisticsConfig c;   // xCurve/yCurve keep the standard Win10 "Enhance pointer precision" defaults
    int speed = 10;
    if (SystemParametersInfo(SPI_GETMOUSESPEED, 0, &speed, 0)) c.sliderMult = PointerSpeedMultiplier(speed);
    int mp[3] = { 0, 0, 0 };
    if (SystemParametersInfo(SPI_GETMOUSE, 0, mp, 0)) c.accelEnabled = (mp[2] != 0);
    return c;
}

// One magnifier tick: advance zoom, hot-reload config, then pan/draw via the render engine.
// Pure of any pacing wait - the caller paces. Safe to call from the main loop or from a
// WM_TIMER during a modal loop.
static void RunTick(TickState& t) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double dt = double(now.QuadPart - t.prev.QuadPart) / double(t.freq.QuadPart);
    t.prev = now;

    // Config hot-reload. A directory-change notification tells us WHEN to re-check magnifier.ini,
    // so the idle render thread does NO per-second filesystem stat (the old 1 Hz GetFileAttributesExW
    // poll caused a ~1s frametime spike under AV/disk contention). WaitForSingleObject(h, 0) is a
    // non-blocking in-process check; we stat + reload only when the exe dir actually changed. Falls
    // back to the old ~1s timed poll if the watch handle is unavailable.
    bool checkConfig = false;
    if (t.configWatch && t.configWatch != INVALID_HANDLE_VALUE) {
        // Poll the watch handle ~4x/s, not every tick: WaitForSingleObject is a kernel transition,
        // and at 144Hz while zoomed that's ~144 needless syscalls/s. Config edits are user-initiated
        // and rare, so ~250ms reload latency is imperceptible (#70).
        t.sinceCheck += dt;
        if (t.sinceCheck >= 0.25) {
            t.sinceCheck = 0.0;
            if (WaitForSingleObject(t.configWatch, 0) == WAIT_OBJECT_0) {
                checkConfig = true;
                // Re-arm for the next change. If that fails (e.g. the watched dir vanished), close
                // the now-useless handle (don't leak it) and drop to INVALID so the timed-poll
                // fallback re-engages instead of silently never reloading.
                if (!FindNextChangeNotification(t.configWatch)) {
                    FindCloseChangeNotification(t.configWatch);
                    t.configWatch = INVALID_HANDLE_VALUE;
                }
            }
        }
    } else {
        t.sinceCheck += dt;
        if (t.sinceCheck > 1.0) { t.sinceCheck = 0.0; checkConfig = true; }
    }
    if (checkConfig) {
        unsigned long long m = ConfigMTime(t.iniPath);
        if (m != t.lastMtime) {
            t.lastMtime = m;
            Config nc = LoadConfig(t.iniPath);
            // Re-bind the hook's button mapping if the user changed it via the config UI; without
            // this the hook would keep firing the OLD button (the new VK works via GetAsyncKeyState
            // but the mouse mapping is captured once in g_input.start at app launch).
            if (nc.zoomInButton != t.cfg.zoomInButton || nc.zoomOutButton != t.cfg.zoomOutButton
             || nc.zoomInButton2 != t.cfg.zoomInButton2 || nc.zoomOutButton2 != t.cfg.zoomOutButton2) {
                g_input.setButtons(nc.zoomInButton, nc.zoomInButton2,
                                   nc.zoomOutButton, nc.zoomOutButton2);
            }
            // Re-bind the keyboard hook's tracked/swallowed keys when any keyboard zoom/recenter
            // bind changed (else the hook keeps swallowing the OLD key and ignores the new one).
            if (nc.zoomInVk != t.cfg.zoomInVk || nc.zoomOutVk != t.cfg.zoomOutVk
             || nc.zoomInVk2 != t.cfg.zoomInVk2 || nc.zoomOutVk2 != t.cfg.zoomOutVk2
             || nc.recenterVk != t.cfg.recenterVk || nc.cursorLockVk != t.cfg.cursorLockVk
             || nc.swapModelVk != t.cfg.swapModelVk) {
                g_input.setKeys(nc.zoomInVk, nc.zoomInVk2, nc.zoomOutVk, nc.zoomOutVk2, nc.recenterVk,
                                nc.cursorLockVk, nc.swapModelVk);
            }
            if (nc.hideCursorVk != t.cfg.hideCursorVk || nc.hideCursorMods != t.cfg.hideCursorMods) {
                RegisterHideCursorHotkey(t.hwnd, nc.hideCursorVk, nc.hideCursorMods);
            }
            if (nc.quickZoomHotkeyMode != t.cfg.quickZoomHotkeyMode
             || nc.quickZoomVk != t.cfg.quickZoomVk || nc.quickZoomMods != t.cfg.quickZoomMods) {
                RegisterQuickZoomHotkey(t.hwnd, (nc.quickZoomHotkeyMode && nc.quickZoomVk) ? nc.quickZoomVk : 0,
                                        nc.quickZoomMods);
            }
            t.cfg = nc;   // pick up renderer knobs (smoothing, filter, cursor scale, zoom speed)
            t.zoom = ZoomController(1.0, nc.maxLevel);
            double ocx = t.mapper.centerX(), ocy = t.mapper.centerY();   // preserve position
            t.mapper = CursorMapper(t.mon.w, t.mon.h, nc.cursorSmoothing);
            t.mapper.reset(ocx, ocy);
        }
    }

    // Effective held state = mouse side-button (set by the hook/raw input) OR keyboard key held.
    // Lets users without side-buttons zoom from the keyboard. When the LL keyboard hook is active it
    // is the authority for bound-key down-state (a swallowed key never appears in GetAsyncKeyState),
    // so read keyPressed(); otherwise (hook install failed / WIND_NOHOOK) fall back to polling.
    const bool kbHook = g_input.kbHookActive();
    auto keyDown = [&](int vk) {
        if (vk == 0) return false;
        if (kbHook && g_input.isBoundKey(vk)) return g_input.keyPressed(vk);
        return (GetAsyncKeyState(vk) & 0x8000) != 0;
    };
    // Modifier mask: bit 1=Ctrl, 2=Alt, 4=Shift, 8=Win. 0 = no modifiers required. Extra modifiers
    // never disqualify (so a "Ctrl+F1" combo still fires when Ctrl+Shift+F1 is held).
    auto modsHeld = [](int mods) {
        if ((mods & 1) && !(GetAsyncKeyState(VK_CONTROL) & 0x8000)) return false;
        if ((mods & 2) && !(GetAsyncKeyState(VK_MENU)    & 0x8000)) return false;
        if ((mods & 4) && !(GetAsyncKeyState(VK_SHIFT)   & 0x8000)) return false;
        if ((mods & 8) && !((GetAsyncKeyState(VK_LWIN) & 0x8000) || (GetAsyncKeyState(VK_RWIN) & 0x8000))) return false;
        return true;
    };
    auto comboHeld = [&](int vk, int mods) { return keyDown(vk) && modsHeld(mods); };
    bool inHeld  = g_input.state().inHeld.load()
        || comboHeld(t.cfg.zoomInVk,  t.cfg.zoomInMods)
        || comboHeld(t.cfg.zoomInVk2, t.cfg.zoomInMods2);
    bool outHeld = g_input.state().outHeld.load()
        || comboHeld(t.cfg.zoomOutVk,  t.cfg.zoomOutMods)
        || comboHeld(t.cfg.zoomOutVk2, t.cfg.zoomOutMods2);
    // Apply the live zoom profile every frame (free hot-reload; setProfile does not reset level).
    // (The old transform <=1.0x ramp-speed cap was a blind TDR mitigation; the resets were
    // root-caused elsewhere - issue #148 - so the user's configured speed applies everywhere.)
    t.zoom.setProfile(t.cfg.zoomInSpeed, t.cfg.zoomOutSpeed, t.cfg.smoothZoom != 0,
                      t.cfg.smoothZoomAccel, t.cfg.smoothZoomRamp);
    // Quick-zoom trigger. Modifier mode (quickZoomHotkeyMode==0): hold the configured modifier
    // (Ctrl/Alt/Shift; "None" = off) and tap a zoom key. While the modifier is held it toggles quick
    // zoom (below) instead of hold-zooming, so suppress the hold-zoom direction (the toggle snaps the
    // level). Hotkey mode (==1): a dedicated hotkey toggles it and the modifier is inert here.
    bool hotkeyMode = t.cfg.quickZoomHotkeyMode != 0;
    const std::string& qzMod = t.cfg.quickZoomModifier;
    int quickZoomModVk = VK_CONTROL;
    if      (_stricmp(qzMod.c_str(), "alt")   == 0) quickZoomModVk = VK_MENU;
    else if (_stricmp(qzMod.c_str(), "shift") == 0) quickZoomModVk = VK_SHIFT;
    bool modifierActive = !hotkeyMode && _stricmp(qzMod.c_str(), "none") != 0;
    bool modKeyDown = modifierActive && (GetAsyncKeyState(quickZoomModVk) & 0x8000) != 0;
    t.zoom.setDirection(modKeyDown ? ZoomDir::None : ResolveDirection(inHeld, outHeld));
    // Clamp the dt fed to the zoom so a single long tick (cold first capture, alt-tab, any hitch)
    // can't jump the zoom level mid-ramp - it should always ease in/out at a steady rate regardless
    // of frame-time spikes. Raw dt is kept below for the diagnostics block (which must see true
    // hitches) and the config-poll fallback. Normal ~7ms frames are unaffected.
    const double kMaxZoomDt = 0.05;   // 50ms (~7 frames at 144Hz)
    t.zoom.tick(dt < kMaxZoomDt ? dt : kMaxZoomDt);
    // Recenter on a recenterVk key press (rising edge).
    bool recenter = false;
    bool recenterDown = keyDown(t.cfg.recenterVk);
    if (recenterDown && !t.recenterKeyWasDown) recenter = true;
    t.recenterKeyWasDown = recenterDown;
    // Swap the magnifier model on the swapModelVk rising edge (works zoomed or idle). This writes the
    // flipped model to the ini and relaunches Wind; the relaunch evicts this instance, so nothing
    // after this in the tick matters once it fires. Processed unconditionally (not gated on zoom).
    bool swapDown = keyDown(t.cfg.swapModelVk);
    // Arm only after the key has been observed UP once. A relaunch builds a fresh TickState, so a key
    // still physically held across the restart would otherwise read as a new rising edge and swap
    // again (a render<->transform restart storm while held). Requiring an observed release first means
    // a held key produces exactly one swap; a plain tap is unaffected.
    if (!swapDown) t.swapArmed = true;
    if (t.swapArmed && swapDown && !t.swapKeyWasDown) SwapModelAndRelaunch(t.iniPath, t.cfg.model);
    t.swapKeyWasDown = swapDown;
    // Inspect mode: toggle on the bound key's rising edge (works at any zoom). The crosshair is
    // overlay-drawn (render_engine draws the crosshair sprite when cursorLocked is set); the active
    // block below freezes the real cursor (1px ClipCursor) and roams a raw-driven look point.
    bool lockDown = keyDown(t.cfg.cursorLockVk);
    if (lockDown && !t.lockKeyWasDown) {
        if (t.model->supportsInspect()) {
            // Snapshot cursor visibility at the toggle edge, BEFORE this tick's active block hides it:
            // at 1x the only thing that can have hidden the cursor is the foreground app, so a
            // not-showing cursor is the mouselook-gameplay tell for game-inspect (issue #144).
            CURSORINFO ci{}; ci.cbSize = sizeof(ci);
            t.inspectCursorWasShowing = GetCursorInfo(&ci) ? (ci.flags & CURSOR_SHOWING) != 0 : true;
            t.cursorLock.toggle();
        } else {
            // Magnify model: Windows Magnifier owns the view and cursor; no freeze+reticle exists.
            wind::Log(wind::LogLevel::Info, "inspect", "Inspect not available in the magnify model");
        }
    }
    t.lockKeyWasDown = lockDown;
    // Tell the mouse hook whether Inspect is on (so it swallows real clicks and routes them to the look
    // point - see the commitButton drain in the active block). Published every tick (also clears on off).
    // gameFreeze (issue #148) reuses the same swallow-and-route path: while the cursor is frozen a
    // real click would land at the frozen point, so the hook eats it and the tick fires it at the
    // aim point instead - identical contract to Inspect.
    g_input.state().inspectActive.store(t.cursorLock.locked() || t.gameFreeze,
                                        std::memory_order_relaxed);
    // Magnify model drives its own zoom natively (Windows Magnifier, wheel notches): feed it the
    // held direction and bypass the ENTIRE level pipeline below - the ZoomController stays at 1x,
    // the overlay never activates, quick zoom / recenter / mapper never run. (The side-button
    // diagnostics block at the bottom is skipped too; the magnify category logs direction edges.)
    if (t.model->selfDrivenZoom()) {
        int rdx, rdy; g_input.drainRaw(rdx, rdy);            // keep the raw accumulator drained
        t.model->nativeZoomTick((inHeld ? 1 : 0) - (outHeld ? 1 : 0), t.cfg);
        t.prevInHeld = inHeld; t.prevOutHeld = outHeld;
        t.prevLvl = 1.0; t.prevActive = false; t.prevInspect = false;
        return;
    }
    // Hide-cursor hotkey is registered via RegisterHotKey (WndProc WM_HOTKEY toggles cursorHidden);
    // this both suppresses the key from reaching other apps and gives rising-edge semantics for
    // free (MOD_NOREPEAT). No polled check needed here.
    // Quick zoom fires from either trigger: the dedicated hotkey (hotkey mode -> WM_HOTKEY set the
    // flag) OR the modifier held + a rising edge of either zoom key (modifier mode). The snap flows
    // into the SAME-tick zoom-in/out transitions below (which key off lvl vs prevLvl).
    // prevInHeld/prevOutHeld update every tick (outside the gate) so re-enabling can't fire a stale edge.
    bool inEdge  = inHeld  && !t.prevInHeld;
    bool outEdge = outHeld && !t.prevOutHeld;
    t.prevInHeld = inHeld; t.prevOutHeld = outHeld;
    bool hotkeyTrigger = t.quickZoomHotkey.exchange(false);   // always consume (only set in hotkey mode)
    bool modZoomTrigger = modKeyDown && (inEdge || outEdge);  // modKeyDown implies modifier mode + enabled
    if (hotkeyTrigger || modZoomTrigger) {
        QuickZoomResult qr = ApplyQuickZoom(t.zoom.level(), t.quickZoomStored,
                                            t.cfg.quickZoomDefault, t.cfg.maxLevel);
        t.zoom.setLevel(qr.newLevel);
        t.quickZoomStored = qr.newStored;
    }
    double lvl = t.zoom.level();
    // tdrTest=1 (issue #148 harness): clamp transform GAME sessions to 8x - the field crashes
    // were level-dependent (mid zoom fine, max zoom fatal); this probes the threshold in vivo.
    if (t.cfg.tdrTest == 1 && t.gameFreeze && lvl > 8.0) {
        t.zoom.setLevel(8.0);
        lvl = 8.0;
    }

    int rawDx, rawDy; g_input.drainRaw(rawDx, rawDy);

    bool zoomed = lvl > 1.0;
    bool inspect = t.cursorLock.locked();
    bool active = zoomed || inspect;                 // overlay runs while zoomed OR Inspect-frozen

    if (active) {
        bool enterActive  = !t.prevActive;            // idle -> active (overlay just turned on)
        if (enterActive && t.mTransform) {
            // Hybrid engine pick, per zoom-in session: fullscreen app foreground on the primary
            // monitor -> transform path (compositor-internal, game-smooth); else render model.
            // Only ever swapped here, at the idle->active edge, so every activation's teardown
            // calls route to the same engine that activated.
            const bool fs = ForegroundCoversMonitor(t.mon);
            // Covering alone also matches MAXIMIZED desktop apps (documented trap); a GAME is a
            // BORDERLESS cover (WS_POPUP fullscreen, no caption) - F11/fullscreen video too,
            // which also prefers the transform path (DRM-safe, compositor-smooth).
            HWND fgw = GetForegroundWindow();
            const bool borderless = fgw && !(GetWindowLongPtrW(fgw, GWL_STYLE) & WS_CAPTION);
            const bool primaryHere = t.mon.x == 0 && t.mon.y == 0;
            // Churny apps (learned cursor-shape churners, issue #148) always get render.
            // tdrTest>0 (field-test harness) bypasses the list to force transform experiments.
            // (The driver's far-right high-zoom TDR is handled INSIDE the transform session by
            // the mapper's pan wall - see setMaxSourceLeft below - so games keep the transform
            // path at every level; a 9x engine-crossover was tried and its handover flash was
            // worse than the wall.)
            IMagnifierModel* pick = (fs && borderless && primaryHere &&
                                     (t.cfg.tdrTest > 0 || !IsChurnyFg(fgw)))
                                        ? t.mTransform : t.mRender;
            if (pick && pick != t.model) t.model = pick;
        }
        bool inspectEnter = inspect && !t.prevInspect;
        if (enterActive) {
            t.outlineIdleSec = 0.0;   // each activation starts with the outline fully shown
            // Follow the cursor's monitor (multiMonitor on, only when zoomed). Only reconfigure when
            // it actually changed; retarget() returns false on multi-GPU/failure, in which case we keep
            // the current monitor. The overlay is still at alpha 0 here, so a move never flashes.
            if (zoomed && t.cfg.multiMonitor) {
                MonitorTarget nt = MonitorUnderCursor();
                if (!SameMonitor(nt, t.mon) && t.model->retarget(nt)) {
                    t.mon = nt;
                    t.mapper = CursorMapper(nt.w, nt.h, t.cfg.cursorSmoothing);
                    int nhz = DetectRefreshHz(nt.device);   // pace off the new monitor's refresh (#74)
                    if (nhz > 0) t.hz = nhz;
                }
            }
            t.vbounds = QueryVirtualBounds();   // refresh cached clip-detect bounds (topology may have changed)
            POINT pt; GetCursorPos(&pt);
            t.mapper.reset(pt.x - t.mon.x, pt.y - t.mon.y);   // virtual -> local monitor coords
            t.lastSetVirtual = pt;        // baseline for the OS-cursor delta (first delta = 0)
            t.detector.reset();           // start free
            // Issue #148 TDR root cause: cursor-position updates racing fullscreen-transform writes
            // in DWM/nvlddmkm reset the driver (repro-proven; the per-tick weld was the worst case,
            // a real hand's HID stream is the second). Transform cursor policy by session type:
            //  - GAME (borderless cover): FREEZE the cursor (1px clip) - zero position updates from
            //    any source, so zero races. Raw mickeys still pan (LockDetector reads the clip as
            //    locked); the sprite marks the aim point; clicks are swallowed and re-fired there.
            //  - DESKTOP: FOLLOW - cursor stays visible (DWM magnifies it), the lens tracks it,
            //    hover and clicks are native-correct. Desktop never TDR'd all night.
            // Only the render model still hides + welds (no DWM magnification there - safe).
            if (dynamic_cast<TransformModel*>(t.model)) {
                // With MPO DISABLED the game-session freeze is unnecessary: the freeze guarded
                // cursor-position updates racing the MPO plane programming, and a REAL hand
                // moving the cursor was always safe (native-Magnifier control test; only
                // INJECTED absolute placement is independently toxic). So MPO off -> games get
                // the full FOLLOW design like the desktop: visible cursor, native hover,
                // clicks, and drags. MPO on -> freeze + pan wall remain the safe fallback.
                HWND ffg = GetForegroundWindow();
                t.gameFreeze = !g_mpoDisabled && ForegroundCoversMonitor(t.mon) && ffg &&
                               !(GetWindowLongPtrW(ffg, GWL_STYLE) & WS_CAPTION);
                if (t.gameFreeze) {
                    POINT fp; GetCursorPos(&fp);
                    t.freezePoint = fp;
                    t.clickReleaseTicks = 0;
                    RECT fz{ fp.x, fp.y, fp.x + 1, fp.y + 1 };
                    ClipCursor(&fz);
                    t.model->hideSystemCursor(true);
                    t.freezeExe = ExeNameOf(ffg);
                    t.churnCount = 0; t.churnWinStart = GetTickCount64(); t.lastFgCursor = nullptr;
                    t.freezeStealPending = (t.cfg.tdrTest == 3);
                    wind::Log(wind::LogLevel::Info, "freeze",
                              "game freeze ENGAGED at (%ld,%ld)", fp.x, fp.y);
                } else {
                    // FOLLOW session: both system-cursor copies stay VISIBLE. (Hiding the raw
                    // copy was tried: MagShowSystemCursor has no raw-only mode - FALSE removes
                    // the magnified copy too, and a sprite replacement re-adds machinery. The
                    // double is tolerable at low cursorSmoothing; revisit only if field
                    // feedback demands it.)
                    wind::Log(wind::LogLevel::Info, "freeze",
                              "transform session WITHOUT freeze (follow)");
                }
            } else {
                t.gameFreeze = false;
                t.model->hideSystemCursor(true);
            }
            t.model->onActivate();       // grab a live frame, not a stale cached one
        }
        if (inspectEnter) {
            // Freeze the real cursor where it is; the look point (mapper center) starts there.
            POINT pt; GetCursorPos(&pt);
            t.frozenCursor = pt;
            t.clickReleaseTicks = 0;   // start frozen (clear any stale click-release window)
            // Match the desktop cursor speed: snapshot the OS pointer-speed/accel and baseline the
            // cooked accumulator + sub-pixel carry so the first tick after entry pans by zero.
            g_input.setBallistics(ReadMouseBallistics());
            double cbx, cby; g_input.drainCooked(cbx, cby); (void)cbx; (void)cby;
            t.inspectPanRemX = 0.0; t.inspectPanRemY = 0.0;
            t.mapper.reset(pt.x - t.mon.x, pt.y - t.mon.y);
            t.lastSetVirtual = pt;
            RECT fz{ pt.x, pt.y, pt.x + 1, pt.y + 1 };
            ClipCursor(&fz);
            t.model->hideSystemCursor(true);   // hide the real cursor; we draw the crosshair
            t.model->onActivate();
            // Game-inspect (issue #144): if a mouselook game holds the mouse, the freeze alone is
            // not enough - its raw-input camera still receives every mickey. Steal foreground to
            // the invisible helper so the game stops getting input. Deferred via
            // inspectStealPending: the reveal logic later this tick must still see the GAME as
            // foreground (ForegroundCoversMonitor decides the composite-gated reveal).
            t.inspectGame = wind::ShouldGameInspect(zoomed, t.detector.locked(),
                                                    t.inspectCursorWasShowing);
            if (t.inspectGame) {
                t.inspectPrevFg = GetForegroundWindow();
                t.inspectStealPending = true;
                wind::Log(wind::LogLevel::Info, "inspect",
                          "game-inspect engaged (zoomed=%d detLocked=%d cursorShown=%d)",
                          (int)zoomed, (int)t.detector.locked(), (int)t.inspectCursorWasShowing);
            }
        }
        bool inspectExit = !inspect && t.prevInspect;   // Inspect just turned off but overlay stays (zoomed)
        if (inspectExit) {
            EndGameInspect(t);   // hand foreground back to the game before resuming normal follow
            if (dynamic_cast<TransformModel*>(t.model)) {
                if (t.gameFreeze) {
                    // Game session continues: stay frozen (the cursor is at the Inspect freeze
                    // point) - just adopt it as the freeze point and keep the clip + hidden cursor.
                    POINT pt; GetCursorPos(&pt);
                    t.freezePoint = pt;
                    RECT fz{ pt.x, pt.y, pt.x + 1, pt.y + 1 };
                    ClipCursor(&fz);
                    t.mapper.reset(pt.x - t.mon.x, pt.y - t.mon.y);
                    t.lastSetVirtual = pt;
                } else {
                    // FOLLOW: NEVER SetCursorPos while the fullscreen transform is live
                    // (the TDR). Resume the lens AT the unfrozen cursor and show it again.
                    ClipCursor(nullptr);
                    POINT pt; GetCursorPos(&pt);
                    t.mapper.reset(pt.x - t.mon.x, pt.y - t.mon.y);
                    t.lastSetVirtual = pt;
                    t.model->hideSystemCursor(false);
                }
            } else {
                ClipCursor(nullptr);
                POINT lp{ (int)(t.mapper.centerX() + 0.5) + t.mon.x, (int)(t.mapper.centerY() + 0.5) + t.mon.y };
                SetCursorPos(lp.x, lp.y);                // warp the real cursor to the look point
                t.lastSetVirtual = lp;
            }
        }
        if (recenter) { POINT pt; GetCursorPos(&pt); t.mapper.reset(pt.x - t.mon.x, pt.y - t.mon.y); t.lastSetVirtual = pt; }
        // Resolve the pan delta. FREE: the OS cursor's own motion since we last placed it - Windows'
        // pointer acceleration is already applied, so we auto-match the real cursor (DPI/accel), then
        // scale by cursorSensitivity as a speed knob (1.0 = exact match, the default). LOCKED: a game
        // has the cursor clipped/recentered, so pan from raw mickeys scaled by the same cursorSensitivity
        // (acceleration doesn't apply to relative-mouse game input). INSPECT: the real cursor is frozen
        // in place, so its delta is irrelevant; the look point roams from raw mickeys instead.
        POINT cur; GetCursorPos(&cur);
        int curDx = cur.x - t.lastSetVirtual.x;
        int curDy = cur.y - t.lastSetVirtual.y;
        int dx, dy;
        if (inspect) {
            // The OS cursor is frozen, so pan the look point from the COOKED mickeys - Windows'
            // pointer-speed + acceleration applied per packet (see mouse_ballistics) - not raw
            // counts, so the look point moves at the same speed/DPI as the desktop cursor.
            // cursorSensitivity stays a user multiplier on top; carry the sub-pixel remainder so
            // slow precise motion is not quantized away.
            double cdx, cdy; g_input.drainCooked(cdx, cdy);
            t.inspectPanRemX += cdx * t.cfg.cursorSensitivity;
            t.inspectPanRemY += cdy * t.cfg.cursorSensitivity;
            dx = (int)t.inspectPanRemX; t.inspectPanRemX -= dx;   // truncate toward zero, carry the rest
            dy = (int)t.inspectPanRemY; t.inspectPanRemY -= dy;
        } else {
            RECT clip{}; GetClipCursor(&clip);
            const VirtualBounds& vb = t.vbounds;   // cached at activation (see QueryVirtualBounds)
            bool clipConfined = clip.left > vb.x || clip.top > vb.y ||
                                clip.right < vb.x + vb.w || clip.bottom < vb.y + vb.h;
            bool locked = t.detector.update(clipConfined,
                                            std::abs(rawDx) + std::abs(rawDy),
                                            std::abs(curDx) + std::abs(curDy));
            if (t.gameFreeze) locked = true;   // frozen cursor: raw mickeys are the only pan source
            if (locked) {
                dx = (int)std::lround(rawDx * t.cfg.cursorSensitivity);
                dy = (int)std::lround(rawDy * t.cfg.cursorSensitivity);
            } else if (dynamic_cast<TransformModel*>(t.model)) {
                // FOLLOW design (issue #148): the transform model never places the cursor, so the
                // lens must track the REAL cursor 1:1. cursorSensitivity is intentionally NOT
                // applied here - scaling would desync the lens center from the visible cursor.
                dx = curDx;
                dy = curDy;
            } else {
                dx = (int)std::lround(curDx * t.cfg.cursorSensitivity);   // auto-matched OS delta, speed-scaled
                dy = (int)std::lround(curDy * t.cfg.cursorSensitivity);
            }
        }
        // Defensive: bound one tick's pan to the monitor span so a stray cursor jump (e.g. the OS
        // cursor briefly escaping to another monitor) cannot teleport the lens. cx_ also clamps.
        if (dx >  t.mon.w) dx =  t.mon.w; else if (dx < -t.mon.w) dx = -t.mon.w;
        if (dy >  t.mon.h) dy =  t.mon.h; else if (dy < -t.mon.h) dy = -t.mon.h;
        // Pan wall (issue #148 final fix): transform GAME sessions keep the source rect's left
        // edge under the driver-safe bound; the far-right strip becomes smoothly unreachable
        // above ~9.3x instead of resetting the GPU. Desktop/F11 transform sessions are
        // unrestricted (field-clean at full range). Y never overflows on this geometry.
        t.mapper.setMaxSourceLeft((t.gameFreeze && !g_mpoDisabled && t.cfg.tdrTest != 4)
                                      ? kMaxSafeTxMagnitude / lvl : -1.0);
        MapResult r = t.mapper.update(dx, dy, lvl);
        // Inspect click-to-look-point: the hook swallowed real click(s) and handed us per-button counts
        // (counts, not a flag, so a fast double-click before this drains isn't lost). Fire a clean ABSOLUTE
        // click at the crosshair (mapper center = look point) per pending press, so each lands where you
        // aim, at any zoom. ABSOLUTE coords are immune to the re-freeze SetCursorPos below, and an absolute
        // injected move is skipped by the raw accumulator (WM_INPUT ignores MOUSE_MOVE_ABSOLUTE), so the
        // look point isn't disturbed. Inspect stays on; the cursor re-freezes at frozenCursor afterwards.
        int nLeft  = g_input.state().commitLeft.exchange(0);
        int nRight = g_input.state().commitRight.exchange(0);
        // Game-inspect: drain but DISCARD clicks. A synthesized click over the game window would
        // re-activate it (mouse clicks activate), re-engaging mouselook mid-inspect; with the game
        // backgrounded and cursorless there is nothing meaningful to click anyway.
        if (nLeft + nRight > 0 && !t.inspectGame) {
            ClipCursor(nullptr);       // release the 1px freeze so the absolute click can reach the look point
            t.clickReleaseTicks = 2;   // ...and keep it released a couple ticks before re-freezing (below)
            const VirtualBounds& vb = t.vbounds;   // cached at activation; equals the SM_*VIRTUALSCREEN metrics
            if (vb.w > 1 && vb.h > 1) {
                int lx = r.clickDesktopX + t.mon.x, ly = r.clickDesktopY + t.mon.y;
                LONG ax = (LONG)((lx - vb.x) * 65535.0 / (vb.w - 1) + 0.5);
                LONG ay = (LONG)((ly - vb.y) * 65535.0 / (vb.h - 1) + 0.5);
                auto fireClicks = [&](DWORD downF, DWORD upF, int count) {
                    for (int k = 0; k < count; ++k) {
                        INPUT clk[3] = {};
                        for (int i = 0; i < 3; ++i) { clk[i].type = INPUT_MOUSE; clk[i].mi.dx = ax; clk[i].mi.dy = ay; }
                        clk[0].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
                        clk[1].mi.dwFlags = downF | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
                        clk[2].mi.dwFlags = upF   | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
                        SendInput(3, clk, sizeof(INPUT));
                    }
                };
                fireClicks(MOUSEEVENTF_LEFTDOWN,  MOUSEEVENTF_LEFTUP,  nLeft);
                fireClicks(MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_RIGHTUP, nRight);
                if (t.gameFreeze && !inspect) {
                    // The injected absolute move races transform writes (issue #148 TDR class), so
                    // SERIALIZE: skip writes for a couple ticks around it, and re-freeze AT the
                    // clicked aim point - hover is then correct there until the next pan.
                    t.freezePoint.x = lx;
                    t.freezePoint.y = ly;
                    t.freezePauseTicks = 3;
                }
            }
        }
        // Fullscreen-game tell for the per-tick perf levers (issue #148): crop the capture copy to
        // the magnified region (gameCrop), skip the periodic topmost backstop, and optionally cap
        // our own present rate (gameFpsCap). Two cheap user32 reads per tick; also reused by the
        // enterActive reveal gating below (which needed the same answer anyway).
        const bool fsGame = ForegroundCoversMonitor(t.mon);
        // Instant hybrid switch (always on): re-pick the engine WHILE ZOOMED when the foreground
        // changes, handing over mid-session. The controller and mapper are untouched, so the
        // zoom level and lens position carry across the swap (render 8x -> tab into a game ->
        // transform 8x). Inspect sessions are never switched under.
        if (t.mTransform && !enterActive && !inspect) {
            HWND fgw2 = GetForegroundWindow();
            const bool fgIsStealer = fgw2 && fgw2 == g_focusStealer;   // tdrTest=3 holds foreground
            const bool borderless2 = fgw2 && !(GetWindowLongPtrW(fgw2, GWL_STYLE) & WS_CAPTION);
            const bool primary2 = t.mon.x == 0 && t.mon.y == 0;
            IMagnifierModel* want = (fsGame && borderless2 && primary2 &&
                                     (t.cfg.tdrTest > 0 || !IsChurnyFg(fgw2)))
                                        ? t.mTransform : t.mRender;
            if (want && want != t.model && !fgIsStealer) {
                if (t.restAfterReveal) {   // rapid double-switch: settle the previous handover
                    t.restAfterReveal->setActive(false);
                    t.restAfterReveal = nullptr;
                    t.restOverlapTicks = 0;
                }
                IMagnifierModel* old = t.model;
                old->hideSystemCursor(false);
                t.model = want;
                // FOLLOW design (issue #148): only render hides + welds; transform keeps the real
                // cursor visible. On render->transform the cursor reappears exactly at the lens
                // point render welded it to, so the handover is seamless.
                if (dynamic_cast<RenderModel*>(t.model)) t.model->hideSystemCursor(true);
                t.model->onActivate();
                if (auto* rm = dynamic_cast<RenderModel*>(t.model)) {
                    t.revealNeedsComposite = ForegroundCoversMonitor(t.mon);
                    if (t.revealNeedsComposite) rm->primeReveal();
                    t.revealPending = (t.hz > 0 ? t.hz : 60) / 4;
                    if (t.revealPending < 2) t.revealPending = 2;
                    // SEAMLESS HANDOVER (field report: a bare-desktop frame flashed at the 9x
                    // crossover): the transform keeps the screen magnified while the overlay's
                    // evidence-gated reveal is pending, and RESTS A FEW TICKS AFTER the reveal
                    // lands - the rest write and the alpha flip travel different DWM channels,
                    // so a same-tick rest still left a one-composite unmagnified gap. The
                    // overlap's worst case is a ~14ms over-zoom pulse (still magnified content),
                    // never a bare desktop.
                    t.restAfterReveal = old;
                    t.restOverlapTicks = 0;   // armed when the reveal actually fires
                } else {
                    // render -> transform: activate the transform THIS tick, keep the overlay up
                    // for the same short overlap, then drop it.
                    t.model->setActive(true);
                    t.restAfterReveal = old;
                    t.restOverlapTicks = 3;
                }
                wind::Log(wind::LogLevel::Info, "hybrid", "instant switch -> %s (level preserved)",
                          dynamic_cast<RenderModel*>(t.model) ? "render" : "transform");
            }
        }
        // Freeze-session sync (issue #148): entering/leaving a transform GAME session mid-zoom
        // (instant switch above, or an alt-tab under a pure-transform session) applies/releases
        // the cursor freeze. Inspect manages its own clip, so skip while it is on.
        if (!enterActive && !inspect) {
            HWND ffg2 = GetForegroundWindow();
            const bool blz = ffg2 && !(GetWindowLongPtrW(ffg2, GWL_STYLE) & WS_CAPTION);
            bool wantFreeze = !g_mpoDisabled &&
                              dynamic_cast<TransformModel*>(t.model) && fsGame && blz;
            if (ffg2 && ffg2 == g_focusStealer && t.gameFreeze) wantFreeze = true;   // tdrTest=3: we hold fg
            if (wantFreeze && !t.gameFreeze) {
                POINT fp; GetCursorPos(&fp);
                t.freezePoint = fp;
                t.clickReleaseTicks = 0;
                RECT fz{ fp.x, fp.y, fp.x + 1, fp.y + 1 };
                ClipCursor(&fz);
                t.model->hideSystemCursor(true);
                t.gameFreeze = true;
                t.freezeExe = ExeNameOf(ffg2);
                t.churnCount = 0; t.churnWinStart = GetTickCount64(); t.lastFgCursor = nullptr;
                t.freezeStealPending = (t.cfg.tdrTest == 3);
                wind::Log(wind::LogLevel::Info, "freeze",
                          "game freeze engaged mid-zoom at (%ld,%ld)", fp.x, fp.y);
            } else if (!wantFreeze && t.gameFreeze) {
                ClipCursor(nullptr);
                t.gameFreeze = false;
                EndFreezeSteal(t);
                if (dynamic_cast<TransformModel*>(t.model)) {
                    // Still transform, now over the desktop: resume FOLLOW at the cursor (the lens
                    // must sit ON the visible cursor, so a reset here is the correct jump).
                    t.model->hideSystemCursor(false);
                    POINT pt; GetCursorPos(&pt);
                    t.mapper.reset(pt.x - t.mon.x, pt.y - t.mon.y);
                    t.lastSetVirtual = pt;
                }
            }
        }
        // Per-tick render-only overrides go through PresentExtras; the model's present() runs
        // FillRenderParams and applies these on top. ex.outline seeds with the same base value
        // FillRenderParams would compute, so the dwell/idle logic below reads an identical start.
        PresentExtras ex;
        ex.fsGame = fsGame;
        ex.forceCrop = fsGame && t.cfg.gameCrop != 0;
        ex.outline = OutlineVisibleAtLevel(t.cfg, lvl);
        ex.outlineAlpha = 1.0f;
        ex.cursorMode = CursorModeFromCfg(t.cfg);
        ex.cursorLocked = false;
        // Low-zoom dwell: with "only at low zoom" on, show the outline only after the zoom settles
        // at a STABLE level inside the band for kOutlineDwellSec. "Stable" = the level is unchanged
        // since last tick (the controller freezes the level exactly when no zoom direction is held),
        // so an actively-changing level - zooming through the band, or repeatedly nudging in/out -
        // never accumulates: the countdown starts only once you stop on a level in the band. Any
        // level change or leaving the band resets it (OutlineDwellSeconds returns 0 when !inBand).
        // Always-on mode (lowZoomOnly off) is unaffected. The reset also fires on zoom-out to idle
        // via the teardown branch below, so cycling 1x<->in-band can never bank partial dwell.
        if (t.cfg.outline != 0 && t.cfg.outlineLowZoomOnly != 0) {
            const double kOutlineDwellSec = 1.0;
            bool stable = std::fabs(lvl - t.prevLvl) <= 1e-4;   // level held constant => settled
            bool inBand = ex.outline && lvl > 1.0 && stable;
            t.outlineZoneSec = OutlineDwellSeconds(inBand, t.outlineZoneSec, dt, kOutlineDwellSec);
            if (t.outlineZoneSec < kOutlineDwellSec) ex.outline = false;   // not dwelled long enough yet
        } else {
            t.outlineZoneSec = 0.0;   // keep ready for when the cutoff is toggled on mid-session
        }
        // Idle-hide fade: when enabled and the outline is visible, accumulate idle time (reset on
        // any hand motion - free OS-cursor delta or raw mickeys), then map it to the fade alpha.
        // dt is the per-tick elapsed time computed at the top of RunTick. Fade duration is 0.3s.
        const bool outlineMoved = (std::abs(curDx) + std::abs(curDy) + std::abs(rawDx) + std::abs(rawDy)) > 0;
        if (t.cfg.outlineIdleHide && ex.outline) {
            t.outlineIdleSec = outlineMoved ? 0.0 : (t.outlineIdleSec + dt);
            ex.outlineAlpha = (float)OutlineIdleAlpha(t.outlineIdleSec, t.cfg.outlineIdleSeconds, 0.3);
        } else {
            t.outlineIdleSec = 0.0;   // keep ready for when idle-hide is toggled on mid-session
        }
        if (t.cursorHidden) ex.cursorMode = 2;   // hotkey override; CursorModeFromCfg already set 0/1/2 from cfg
        // cursorMode is now final for this tick; derive drawCursor from it so the transform model
        // (which only reads drawCursor, not cursorMode - see magnifier_model.h) also honours
        // cursorVisibility=never and the hide-cursor hotkey. The render model never reads drawCursor
        // (it reads cursorMode directly via FillRenderParams), so this cannot change render behaviour.
        ex.drawCursor = (ex.cursorMode != 2);
        if (inspect) {
            if (t.clickReleaseTicks > 0) {
                // A click was just committed: keep the freeze released for these ticks so the synthesized
                // absolute click reaches the look point (re-clamping to the 1px frozen rect would send the
                // click to the frozen point instead). Leave p.clickDesktop at the look point so renderFrame
                // holds the cursor there for the click; re-freeze once the window elapses.
                --t.clickReleaseTicks;
            } else {
                // Re-assert the freeze (Windows can drop a clip on focus changes) and pin renderFrame's
                // SetCursorPos at the frozen point (a no-op inside the clip). DEDUPED (issue #148):
                // ClipCursor is a win32k cursor-subsystem write - the TDR class under an active
                // fullscreen transform - so it only runs when the read shows the clip was lost.
                RECT have{}; GetClipCursor(&have);
                if (have.left != t.frozenCursor.x || have.top != t.frozenCursor.y ||
                    have.right != t.frozenCursor.x + 1 || have.bottom != t.frozenCursor.y + 1) {
                    RECT fz{ t.frozenCursor.x, t.frozenCursor.y, t.frozenCursor.x + 1, t.frozenCursor.y + 1 };
                    ClipCursor(&fz);
                }
                ex.clickOverride = true;
                ex.clickDesktopX = t.frozenCursor.x;
                ex.clickDesktopY = t.frozenCursor.y;
            }
            ex.cursorLocked = true;        // draw the crosshair at the look point (cursorScreen)
            if (!zoomed) ex.outline = false;   // no lens outline on the 1:1 view at 1x
        } else if (t.gameFreeze) {
            // Transform game session (issue #148): sprite marks the aim point; writes pause while a
            // click's injected move is in flight. The 1px clip is re-asserted ONLY when a read
            // shows it was dropped (focus changes can clear it): ClipCursor is a win32k
            // cursor-subsystem WRITE - the proven TDR class under active magnification - so it
            // must never run per-tick. GetClipCursor is a read and always safe.
            ex.gameFreeze = true;
            ex.pauseWrites = t.freezePauseTicks > 0;
            if (t.freezePauseTicks > 0) --t.freezePauseTicks;
            t.lastFreezeActiveMs = GetTickCount64();
            if (t.cfg.tdrTest == 3 && t.freezeStealPending) {
                // tdrTest=3: background the game while zoomed - it stops receiving input, so
                // its cursor/hover machinery goes quiet (the Snipping Tool effect, issue #144).
                HWND curFg = GetForegroundWindow();
                if (curFg && curFg != g_focusStealer) t.freezePrevFg = curFg;
                if (StealForeground(EnsureFocusStealer(t.mon))) t.freezeStealPending = false;
            }
            // Churn valve (issue #148 trigger 3, rig-proven): the app's cursor SHAPE changing
            // under our transform writes is the driver killer we cannot prevent - detect it
            // (read-only poll) and hand this session to render; the app is remembered so future
            // zoom-ins skip transform entirely. 4+ handle changes inside a rolling second.
            CURSORINFO ci{}; ci.cbSize = sizeof(ci);
            if (GetCursorInfo(&ci)) {
                ULONGLONG nowMs = GetTickCount64();
                if (nowMs - t.churnWinStart > 1000) { t.churnWinStart = nowMs; t.churnCount = 0; }
                if (ci.hCursor != t.lastFgCursor) {
                    t.lastFgCursor = ci.hCursor;
                    if (++t.churnCount >= 4) MarkChurnyApp(t.freezeExe, "cursor churn");
                }
            }
            // NO HOVER SYNC (tried 2026-07-26, REVERTED same night): injecting one absolute
            // move per pan-rest to update the game's hover point TDR'd the driver even with
            // MPO disabled - absolute-placement injection is its own independent trigger, and
            // rest-frequency rolls the dice several times a second (clicks survive only by
            // being rare). The write-pause gaps also read as stutter. Hover therefore follows
            // only on CLICK in game sessions, by design; see docs/ROADMAP.md for alternatives.
            if (t.clickReleaseTicks > 0) {
                --t.clickReleaseTicks;
            } else {
                RECT have{}; GetClipCursor(&have);
                if (have.left != t.freezePoint.x || have.top != t.freezePoint.y ||
                    have.right != t.freezePoint.x + 1 || have.bottom != t.freezePoint.y + 1) {
                    RECT fz{ t.freezePoint.x, t.freezePoint.y,
                             t.freezePoint.x + 1, t.freezePoint.y + 1 };
                    ClipCursor(&fz);
                    wind::Log(wind::LogLevel::Info, "freeze", "clip re-asserted (was dropped)");
                }
            }
        }
        // Game pacing (issue #148): ONLY for the opt-in knobs. The default zoomed path keeps the
        // vsync-locked blocking Present - it is what makes panning smooth (refresh-locked cadence),
        // and at normal GPU priority it blocks a few frames at worst, never wedges. The timer-paced
        // Present(0,0) + fence-gate mode below exists because lowGpuPriority=1 can starve our GPU
        // work for MINUTES under a saturated game (a blocking present then wedges input, teardown,
        // and the cursor restore), and because gameFpsCap needs presents decoupled from ticks. Both
        // trade present cadence for safety/headroom, so they must never engage by default - an
        // earlier build enabled this mode for every "foreground covers the monitor" case (which
        // also matches any MAXIMIZED desktop window) and made panning judder everywhere.
        // While engaged: the main-loop timer paces (t.gamePacing), presents are Present(0,0)
        // (ex.noVsync), and renderFrame skips the whole frame while the previous present hasn't
        // executed on the GPU (ex.gatePresent). Skipped ticks still sample input and advance the
        // mapper. The activation and reveal-pending ticks always attempt a present (the gated
        // reveal depends on frames reaching the redirection surface).
        bool doPresent = true;
        // Reduced-PUSH game mode (issue #148, measured in Foundation): under a game, DWM
        // services our redirected window's presents at only ~78/s while compositing at 144/s -
        // pushing 144 presents/s into that path builds a standing queue, so every Present waits
        // ~a queue's worth with jitter (the stutter). gameFpsCap>0 over a fullscreen game
        // presents every Nth vblank instead (N = ceil(hz/cap)), BELOW the service rate, so the
        // queue stays empty and each present retires on the next composite. Skip ticks block on
        // IDXGIOutput::WaitForVBlank (in the skip branch below), keeping the loop vblank-locked
        // with no timer jitter; input/pan still samples every tick. Activation/reveal ticks
        // always present (the gated reveal needs frames on the redirection surface).
        const bool capVsync = fsGame && zoomed && t.cfg.gameFpsCap > 0 &&
                              t.cfg.vsync != 0 && t.cfg.dwmFlush == 0;
        if (capVsync && !(enterActive || t.revealPending > 0)) {
            int div = (t.hz + t.cfg.gameFpsCap - 1) / t.cfg.gameFpsCap;   // ceil(hz/cap)
            if (div < 1) div = 1;
            if (++t.pushPhase >= div) t.pushPhase = 0;
            if (t.pushPhase != 0) doPresent = false;
        } else {
            t.pushPhase = 0;
        }
        const bool gamePacing = fsGame && zoomed && !capVsync &&
                                (EffectiveGpuPriority(t.cfg) < 0 || t.cfg.gameFpsCap > 0);
        t.gamePacing = gamePacing;
        if (gamePacing) {
            ex.noVsync = true;
            ex.gatePresent = true;
            if (t.cfg.gameFpsCap > 0) {
                const double interval = 1.0 / (double)t.cfg.gameFpsCap;
                t.presentAccum += dt;
                if (enterActive || t.revealPending > 0) {
                    t.presentAccum = 0.0;                   // reveal path: present every tick
                } else if (t.presentAccum >= interval) {
                    t.presentAccum -= interval;
                    if (t.presentAccum > interval) t.presentAccum = interval;  // hitch: no burst catch-up
                } else {
                    doPresent = false;
                }
            }
        } else {
            t.presentAccum = 0.0;
        }
        if (doPresent) {
            t.model->present(r, lvl, t.cfg, t.mon, ex);      // render+present (never blocks the ramp)
        } else if (capVsync) {
            // Reduced-push skip tick: block to the next vblank so the loop cadence stays
            // vblank-locked (Present paces the present ticks, this paces the skips). Fallback
            // sleep if the output can't wait (device transition) so we never spin.
            if (auto* rm = dynamic_cast<RenderModel*>(t.model)) {
                if (!rm->waitVBlank()) Sleep(3);
            }
        }
        // Reveal AFTER the live frame is presented: setVisible flips the layer alpha over the
        // now-current front buffer, so the overlay never shows its retained previous-session
        // frame (the alt-tab "previous window"). capture() also drained to the latest frame.
        // Reveal/prime is render-specific (needs ForegroundCoversMonitor + capture priming); guard it
        // behind the RenderModel downcast. A non-render model just reveals immediately on activation.
        if (auto* rm = dynamic_cast<RenderModel*>(t.model)) {
            if (enterActive) {
                // EVERY reveal is gated on the session's first Present having EXECUTED on the GPU
                // (revealFrameDone: an event query fenced right after Present). The blt into the
                // layered redirection surface is GPU work, but the alpha flip is a CPU call DWM
                // honours at its next composite - under GPU load the flip won that race and DWM
                // composited the surface's RETAINED frame: the last thing the PREVIOUS zoom
                // session presented (issue #140, second mechanism; the first was capture-side).
                // A fullscreen app (game on an independent-flip/MPO plane, issue #90) additionally
                // needs a post-prime composite in the capture (frameCompositedSincePrime), since
                // Desktop Duplication can't see the game until the alpha-1 prime forces DWM to
                // composite it. All non-blocking - the smooth-zoom ramp runs undisturbed;
                // revealPending is only the fallback cap (~250 ms) so nothing can wedge the reveal.
                t.revealNeedsComposite = fsGame;   // same ForegroundCoversMonitor read, this tick
                if (t.revealNeedsComposite) rm->primeReveal();
                t.revealPending = (t.hz > 0 ? t.hz : 60) / 4;
                if (t.revealPending < 2) t.revealPending = 2;
                // Ordinary desktop zoom-in: spin a tiny budget on the fence so the idle-GPU common
                // case still reveals within this same tick (the instant feel is kept); a loaded
                // GPU misses the budget and defers to the per-tick checks below.
                if (!t.revealNeedsComposite && rm->revealFrameDone(3.0)) {
                    rm->setActive(true);
                    t.revealPending = 0;
                    if (t.restAfterReveal) t.restOverlapTicks = 3;   // overlap, then rest
                }
            } else if (t.revealPending > 0) {
                --t.revealPending;
                const bool frameDone  = rm->revealFrameDone();
                const bool composited = !t.revealNeedsComposite || rm->frameCompositedSincePrime();
                if ((frameDone && composited) || t.revealPending == 0) {
                    rm->setActive(true);
                    wind::Log(wind::LogLevel::Info, "render",
                              "deferred reveal: frameDone=%d composited=%d ticksLeft=%d",
                              (int)frameDone, (int)composited, t.revealPending);
                    t.revealPending = 0;
                    if (t.restAfterReveal) t.restOverlapTicks = 3;   // overlap, then rest
                }
            }
        } else if (enterActive) {
            t.model->setActive(true);   // transform: reveal immediately, no capture priming
        }
        // Handover overlap: the outgoing engine rests a few ticks after the incoming one is
        // live, so the crossover never composites a bare unmagnified frame (see instant switch).
        if (t.restAfterReveal && t.restOverlapTicks > 0 && --t.restOverlapTicks == 0) {
            t.restAfterReveal->setActive(false);
            t.restAfterReveal = nullptr;
        }
        // Execute the deferred game-inspect steal now that the reveal logic has read the true
        // foreground, and RE-assert it if the game pulled foreground back mid-inspect (some
        // engines re-grab on their own timers; a user alt-tab to a THIRD app is respected).
        // If the steal fails (unsigned dev build denied by the foreground lock), drop back to
        // normal inspect: the camera moves again, but the state is never inconsistent.
        if (inspect && t.inspectGame) {
            HWND fg = GetForegroundWindow();
            bool wantSteal = t.inspectStealPending || (t.inspectPrevFg && fg == t.inspectPrevFg);
            t.inspectStealPending = false;
            if (wantSteal && !StealForeground(EnsureFocusStealer(t.mon))) {
                wind::Log(wind::LogLevel::Warn, "inspect",
                          "game-inspect: foreground steal failed; falling back to normal inspect");
                t.inspectGame = false;
                t.inspectPrevFg = nullptr;
            }
        }
        // Bookkeeping for next tick's GetCursorPos delta. INSPECT: the real cursor stays frozen, so
        // the baseline is the frozen point. TRANSFORM (follow design, issue #148): nothing was
        // placed - the baseline is where the cursor actually is, so the next delta is purely the
        // user's hand. RENDER: renderFrame SetCursorPos'd the OS cursor to clickDesktop+origin.
        if (inspect) {
            t.lastSetVirtual = t.frozenCursor;
        } else if (dynamic_cast<TransformModel*>(t.model)) {
            t.lastSetVirtual = cur;
        } else {
            t.lastSetVirtual.x = r.clickDesktopX + t.mon.x;
            t.lastSetVirtual.y = r.clickDesktopY + t.mon.y;
        }
    } else if (t.prevActive) {                        // active -> idle: tear the overlay down
        if (t.restAfterReveal) { t.restAfterReveal->setActive(false); t.restAfterReveal = nullptr; }
        t.model->setActive(false);
        t.model->hideSystemCursor(false);
        t.outlineZoneSec = 0.0;                       // zoom-out clears the low-zoom dwell (no banked partial)
        t.gamePacing = false;                         // idle: normal timer pacing
        t.pushPhase = 0;
        t.presentAccum = 0.0;
        if (t.gameFreeze) {
            ClipCursor(nullptr);
            t.gameFreeze = false;
            EndFreezeSteal(t);
            wind::Log(wind::LogLevel::Info, "freeze", "game freeze released (zoom-out)");
            if (!t.prevInspect) {
                // Transform rested to 1.0 in setActive(false) above, so a one-shot placement at
                // the aim point is race-free now and keeps zoom-out continuity (the cursor lands
                // where the user was aiming, exactly like the render model's zoom-out).
                POINT lp{ (int)(t.mapper.centerX() + 0.5) + t.mon.x,
                          (int)(t.mapper.centerY() + 0.5) + t.mon.y };
                SetCursorPos(lp.x, lp.y);
                t.lastSetVirtual = lp;
            }
        }
        if (t.prevInspect) {
            EndGameInspect(t);   // teardown-to-idle exits game-inspect too (foreground returned)
            ClipCursor(nullptr);
            POINT lp{ (int)(t.mapper.centerX() + 0.5) + t.mon.x, (int)(t.mapper.centerY() + 0.5) + t.mon.y };
            SetCursorPos(lp.x, lp.y);                  // resume at the look point
            t.lastSetVirtual = lp;
        }
        t.revealPending = 0;                          // a quick tap may zoom out before the deferred reveal
    } else {
        // Idle: let the transform model release its magnification context shortly after a zoom
        // ends. While a context is alive, DWM composites magnification-aware and every cursor
        // change an app makes costs a re-composite - a game that toggles its pointer on
        // middle-click hitches at 1x (issue #148). Both the active model and hybrid's transform
        // half get the tick (in model=transform the transform IS t.model); others no-op.
        t.model->idleTick();
        if (t.mTransform && t.mTransform != t.model) t.mTransform->idleTick();
    }
    t.prevLvl = lvl;
    t.prevActive = active;
    t.prevInspect = inspect;

    // Diagnostics (issue #113): log the side-button held-state timeline so the intermittent stuck can
    // be diagnosed from the log. On every rise/fall, dump the hook + Raw Input event counters and the
    // held duration; a stuck shows as a rise with no matching fall (and the next event only on
    // re-click). Also WARN once if a hold overstays 6 s (well past any hold-to-zoom, which caps in
    // ~2 s) - that line, with static counters, pinpoints a stuck episode. Edges/overstay only, so
    // Log() is never hit on the per-frame path.
    {
        auto& st = g_input.state();
        auto snap = [&](const char* tag, bool held, bool& prev, double& secs, bool& warned) {
            if (held != prev) {
                // Edge: `secs` still holds the accumulated duration (meaningful on a fall; ~0 on a rise).
                wind::Log(wind::LogLevel::Info, "input",
                          "%sHeld %d->%d held=%.2fs hook[d=%u u=%u dbl=%u / d=%u u=%u dbl=%u] raw[d=%u u=%u / d=%u u=%u] hookActive=%d lvl=%.2f",
                          tag, prev ? 1 : 0, held ? 1 : 0, secs,
                          st.dbgHookDown[1].load(), st.dbgHookUp[1].load(), st.dbgHookDbl[1].load(),
                          st.dbgHookDown[2].load(), st.dbgHookUp[2].load(), st.dbgHookDbl[2].load(),
                          st.dbgRawDown[1].load(), st.dbgRawUp[1].load(),
                          st.dbgRawDown[2].load(), st.dbgRawUp[2].load(),
                          g_input.hookActive() ? 1 : 0, lvl);
                warned = false;            // arm the overstay warning for the next episode
                prev = held;
            } else if (held && !warned && secs > 6.0) {
                wind::Log(wind::LogLevel::Warn, "input",
                          "%sHeld STUCK? held=%.1fs hook[d=%u u=%u dbl=%u / d=%u u=%u dbl=%u] raw[d=%u u=%u / d=%u u=%u] lvl=%.2f",
                          tag, secs,
                          st.dbgHookDown[1].load(), st.dbgHookUp[1].load(), st.dbgHookDbl[1].load(),
                          st.dbgHookDown[2].load(), st.dbgHookUp[2].load(), st.dbgHookDbl[2].load(),
                          st.dbgRawDown[1].load(), st.dbgRawUp[1].load(),
                          st.dbgRawDown[2].load(), st.dbgRawUp[2].load(), lvl);
                warned = true;
            }
            // Accumulate AFTER edge handling so a fall reports the pre-reset duration; cleared at 0 when up.
            secs = held ? (secs + dt) : 0.0;
        };
        bool inHeldNow  = g_input.state().inHeld.load();
        bool outHeldNow = g_input.state().outHeld.load();
        snap("in",  inHeldNow,  t.dbgPrevInHeld,  t.dbgInHeldSec,  t.dbgInOverstayLogged);
        snap("out", outHeldNow, t.dbgPrevOutHeld, t.dbgOutHeldSec, t.dbgOutOverstayLogged);
    }

    // Frame-pacing diagnostics: a 2 s window of loop-interval stats (dt = time between ticks =
    // the on-screen frame interval, since Present(1,0) paces while zoomed). maxDt and the hitch
    // count expose microstutter that an average would hide.
    if (t.cfg.diagnostics) {
        const double target = 1.0 / (t.hz > 0 ? t.hz : 60);
        t.diagSumDt += dt; t.diagFrames++; t.diagAccum += dt;
        if (dt > t.diagMaxDt) t.diagMaxDt = dt;
        if (dt > target * 1.5) t.diagHitches++;
        if (t.diagAccum >= 2.0 && t.diagFrames > 0) {
            // Render/present CPU split from the engine (render model only): avg/max ms building
            // the frame vs blocked inside Present - the present column is where GPU contention
            // with a game shows up (issue #148).
            double rSum = 0, rMax = 0, pSum = 0, pMax = 0; int pf = 0, skips = 0;
            if (auto* rm = dynamic_cast<RenderModel*>(t.model))
                rm->engine().debugPerf(rSum, rMax, pSum, pMax, pf, skips, /*reset=*/true);
            DiagLog("zoom=%.2f frames=%d ~fps=%.0f avgDt=%.2fms maxDt=%.2fms hitches>1.5x=%d "
                    "render(avg=%.2f max=%.2f)ms present(avg=%.2f max=%.2f)ms presented=%d gateSkips=%d",
                    lvl, t.diagFrames, t.diagFrames / t.diagAccum,
                    t.diagSumDt / t.diagFrames * 1000.0, t.diagMaxDt * 1000.0, t.diagHitches,
                    pf > 0 ? rSum / pf : 0.0, rMax, pf > 0 ? pSum / pf : 0.0, pMax, pf, skips);
            t.diagAccum = 0.0; t.diagSumDt = 0.0; t.diagMaxDt = 0.0;
            t.diagFrames = 0; t.diagHitches = 0;
        }
    }
}

// Message-handler: decodes raw mouse movement (survives cursor lock) and routes tray msgs.
// Global panic/quit hotkey id (Ctrl+Alt+Q). Quits cleanly from anywhere - even while the
// render overlay covers the screen and the OS cursor is hidden - so there's always a
// keyboard-only escape. The clean exit path restores the cursor and resets zoom to 1x.
static const int kQuitHotkeyId = 0xB001;
static const int kHideCursorHotkeyId = 0xB002;
static const int kQuickZoomHotkeyId = 0xB003;

// Translate our bit mask (1=Ctrl, 2=Alt, 4=Shift, 8=Win) into Win32 MOD_* flags for RegisterHotKey.
// MOD_NOREPEAT is always added so holding the key fires the hotkey once, not on auto-repeat.
static UINT WinModsFromBitmask(int mods) {
    UINT m = MOD_NOREPEAT;
    if (mods & 1) m |= MOD_CONTROL;
    if (mods & 2) m |= MOD_ALT;
    if (mods & 4) m |= MOD_SHIFT;
    if (mods & 8) m |= MOD_WIN;
    return m;
}

// Hot-reloadable registration of the hide-cursor hotkey. RegisterHotKey suppresses the key from
// reaching other apps and delivers WM_HOTKEY to the owning window. Re-register on config change.
static int g_registeredHideVk = 0;
static int g_registeredHideMods = 0;
static void RegisterHideCursorHotkey(HWND hwnd, int vk, int mods) {
    if (g_registeredHideVk != 0) {
        UnregisterHotKey(hwnd, kHideCursorHotkeyId);
        g_registeredHideVk = 0; g_registeredHideMods = 0;
    }
    if (vk != 0 && RegisterHotKey(hwnd, kHideCursorHotkeyId, WinModsFromBitmask(mods), vk)) {
        g_registeredHideVk = vk; g_registeredHideMods = mods;
    }
}

// Hot-reloadable registration of the quick-zoom hotkey (hotkey mode). Callers pass vk=0 to
// unregister (modifier mode, or hotkey cleared), releasing the global key grab.
static int g_registeredQuickVk = 0;
static int g_registeredQuickMods = 0;
static void RegisterQuickZoomHotkey(HWND hwnd, int vk, int mods) {
    if (g_registeredQuickVk != 0) {
        UnregisterHotKey(hwnd, kQuickZoomHotkeyId);
        g_registeredQuickVk = 0; g_registeredQuickMods = 0;
    }
    if (vk != 0 && RegisterHotKey(hwnd, kQuickZoomHotkeyId, WinModsFromBitmask(mods), vk)) {
        g_registeredQuickVk = vk; g_registeredQuickMods = mods;
    }
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_HOTKEY && wp == kQuitHotkeyId) { PostQuitMessage(0); return 0; }
    if (msg == WM_HOTKEY && wp == kHideCursorHotkeyId) {
        if (g_tick) g_tick->cursorHidden = !g_tick->cursorHidden;
        return 0;
    }
    if (msg == WM_HOTKEY && wp == kQuickZoomHotkeyId) {
        if (g_tick) g_tick->quickZoomHotkey.store(true);   // RunTick consumes it (rising-edge via MOD_NOREPEAT)
        return 0;
    }
    // Keep ticking while a modal loop (the tray menu) owns the thread. The tray sets a timer
    // around TrackPopupMenu; its WM_TIMER lands here so the lens doesn't freeze. (No other
    // WM_TIMER exists in this process.)
    if (msg == WM_TIMER) { if (g_tick) RunTick(*g_tick); return 0; }
    if (msg == WM_INPUT) {
        UINT size = 0;
        GetRawInputData((HRAWINPUT)lp, RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER));
        alignas(8) BYTE buf[128];
        if (size > 0 && size <= sizeof(buf) &&
            GetRawInputData((HRAWINPUT)lp, RID_INPUT, buf, &size, sizeof(RAWINPUTHEADER)) == size) {
            auto* ri = reinterpret_cast<RAWINPUT*>(buf);
            if (ri->header.dwType == RIM_TYPEMOUSE) {
                const RAWMOUSE& m = ri->data.mouse;
                if ((m.usFlags & MOUSE_MOVE_ABSOLUTE) == 0) {
                    AccumulateRaw(g_input, m.lLastX, m.lLastY);
                }
                // Side-button held state. The button DOWN edge stays HOOK-authoritative when the
                // LL hook is active (it owns the swallow/edge logic; writing DOWN here too would
                // double-count and could momentarily disagree with the hook's view) - so DOWN is
                // decoded here only as the WIND_NOHOOK fallback. The button UP, however, is ALWAYS
                // honored from Raw Input: an LL hook can be silently skipped by Windows on a
                // LowLevelHooksTimeout stall, and a dropped XBUTTON UP would otherwise strand the
                // button as held (intermittent stuck-zoom, recovers only on a re-click). Raw Input
                // is delivered through a path NOT subject to that timeout, and a UP can only CLEAR
                // held-state, never set it, so processing it unconditionally is a pure safety net
                // (idempotent with the hook's own clear; never falsely holds). It does not touch the
                // hook's g_swallowedDown record, so swallowing is unaffected.
                USHORT bf = m.usButtonFlags;
                if (bf & RI_MOUSE_BUTTON_4_UP) g_input.setButtonState(1, false);
                if (bf & RI_MOUSE_BUTTON_5_UP) g_input.setButtonState(2, false);
                if (!g_input.hookActive()) {
                    if (bf & RI_MOUSE_BUTTON_4_DOWN) g_input.setButtonState(1, true);
                    if (bf & RI_MOUSE_BUTTON_5_DOWN) g_input.setButtonState(2, true);
                }
                // Diagnostics (issue #113): record whether Raw Input even reports this mouse's side
                // buttons (some mice route them through a vendor HID collection and never raise
                // RI_MOUSE_BUTTON_4/5). Bump counters + log ONLY on a side-button transition, never on
                // a plain move, so this stays off the per-frame path.
                {
                    auto& st = g_input.state();
                    if (bf & RI_MOUSE_BUTTON_4_DOWN) st.dbgRawDown[1].fetch_add(1, std::memory_order_relaxed);
                    if (bf & RI_MOUSE_BUTTON_4_UP)   st.dbgRawUp[1].fetch_add(1, std::memory_order_relaxed);
                    if (bf & RI_MOUSE_BUTTON_5_DOWN) st.dbgRawDown[2].fetch_add(1, std::memory_order_relaxed);
                    if (bf & RI_MOUSE_BUTTON_5_UP)   st.dbgRawUp[2].fetch_add(1, std::memory_order_relaxed);
                    if (bf & (RI_MOUSE_BUTTON_4_DOWN | RI_MOUSE_BUTTON_4_UP |
                              RI_MOUSE_BUTTON_5_DOWN | RI_MOUSE_BUTTON_5_UP))
                        wind::Log(wind::LogLevel::Info, "input", "raw xbtn bf=0x%04x", (unsigned)bf);
                }
            }
        }
        return 0;
    }
    if (Tray::HandleMessage(hwnd, msg, wp, lp)) return 0;
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Force-restore any global input state a previous Wind may have left dirty (cursor hidden, cursor
// confined, a stuck show-count). Safe to call unconditionally at startup and at exit: every call
// is idempotent. This is the net that guarantees a force-killed or crashed predecessor can never
// leave the machine with a hidden/locked cursor - the next launch (and our own exit) heals it.
static void RestoreInputState() {
    ClipCursor(nullptr);                                         // release any cursor confinement
    if (MagInitialize()) { MagShowSystemCursor(TRUE); MagUninitialize(); }   // un-hide the OS cursor
    SystemParametersInfoW(SPI_SETCURSORS, 0, nullptr, SPIF_SENDCHANGE);      // reload system cursors
    for (int i = 0; i < 8 && ShowCursor(TRUE) < 0; ++i) {}       // bump our show-count back to visible
}

// Minimal restore for abnormal CRT exit (atexit / ExitProcess). Non-blocking: must not touch the
// hook thread (its stop() waits, which could hang during teardown); the hook dies with the process
// and only swallows side-buttons anyway. The damaging state (hidden/confined cursor) is undone here.
static void AtExitRestore() { RestoreInputState(); }

// Crash safety net installed BEFORE the magnifier model is constructed. RenderModel hides the OS
// cursor via the process-scoped Magnification API (auto-reverts on process death), and the magnify
// model never touches the cursor, but the SPI_SETCURSORS reload is kept as a general heal for any
// stale cursor scheme a crashed predecessor left behind. Body mirrors render_engine.cpp's
// CursorRestoreFilter (minimal, allocation-light, one-shot via InterlockedExchange, returns
// EXCEPTION_CONTINUE_SEARCH so the default handler still reports the crash). RenderEngine::
// hideSystemCursor installs its own filter on the render path's first cursor hide, which REPLACES
// this one (SetUnhandledExceptionFilter keeps only the latest); that is safe because CursorRestoreFilter
// does the identical restore + crash report, so nothing is lost by the replacement.
static LONG WINAPI EarlyCursorRestoreFilter(EXCEPTION_POINTERS* ep) {
    static LONG s_inHandler = 0;
    if (InterlockedExchange(&s_inHandler, 1)) return EXCEPTION_CONTINUE_SEARCH;
    MagShowSystemCursor(TRUE);           // no-op if the Magnification API was never initialized this run
    ClipCursor(nullptr);                 // never leave the cursor clipped if we crash while Inspect-locked
    SystemParametersInfoW(SPI_SETCURSORS, 0, nullptr, SPIF_SENDCHANGE);   // heals a blanked cursor scheme
    wind::WriteCrashReport(ep);          // minidump + text summary into the log dir
    return EXCEPTION_CONTINUE_SEARCH;   // let the default handler still report the crash
}

// Single-instance startup events route through the unified logger (category "startup").
static void SiLog(const char* msg, unsigned long val) {
    wind::Log(wind::LogLevel::Info, "startup", "%s %lu", msg, val);
}

// Force-kill every OTHER Wind.exe (best effort; OpenProcess may be denied across integrity levels).
static void TerminateOtherWind() {
    const DWORD self = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W pe{ sizeof(pe) };
    if (Process32FirstW(snap, &pe)) {
        do {
            if (pe.th32ProcessID != self && _wcsicmp(pe.szExeFile, L"Wind.exe") == 0) {
                HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
                SiLog("terminate pid", pe.th32ProcessID);
                if (h) { BOOL ok = TerminateProcess(h, 0); SiLog("  terminate ok", ok);
                         if (!ok) SiLog("  terminate err", GetLastError()); CloseHandle(h); }
                else SiLog("  openprocess err", GetLastError());
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
}

// Swap the magnifier model (render <-> transform) by rewriting magnifier.ini's `model` in place and
// relaunching Wind.exe: model is read once at launch, so a restart is the only way to switch it. The
// freshly launched instance runs the single-instance eviction handshake (signals Wind_QuitRequest),
// so THIS instance exits cleanly (cursor restore, tray removal, clip release) - no new IPC. On a
// launch failure the ini is reverted so it never claims a model the live process is not running.
static void SwapModelAndRelaunch(const std::wstring& iniPath, const std::string& currentModel) {
    const std::string flipped = wind::FlipModel(currentModel);
    // Read the ini text (UTF-8). If it can't be read, bail rather than clobber it.
    std::string text;
    { std::ifstream f(iniPath, std::ios::binary);
      if (!f) { wind::Log(wind::LogLevel::Warn, "swap", "ini read failed; swap skipped"); return; }
      text.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()); }
    const std::string updated = wind::UpdateIniText(text, "model", flipped);
    { std::ofstream out(iniPath, std::ios::binary | std::ios::trunc);
      if (!out) { wind::Log(wind::LogLevel::Warn, "swap", "ini write failed; swap skipped"); return; }
      out << updated; }
    wind::Log(wind::LogLevel::Info, "swap", "model %s -> %s; relaunching",
              currentModel.c_str(), flipped.c_str());
    wchar_t exePath[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, exePath, MAX_PATH)) {
        wind::Log(wind::LogLevel::Warn, "swap", "GetModuleFileName failed; reverting ini");
        std::ofstream out(iniPath, std::ios::binary | std::ios::trunc); out << text; return;
    }
    HINSTANCE r = ShellExecuteW(nullptr, L"open", exePath, nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(r) <= 32) {
        // Relaunch failed: stay on the current model and put the ini back so it matches reality.
        wind::Log(wind::LogLevel::Warn, "swap", "relaunch failed (%lld); reverting ini",
                  static_cast<long long>(reinterpret_cast<INT_PTR>(r)));
        std::ofstream out(iniPath, std::ios::binary | std::ios::trunc); out << text;
    }
    // On success, do nothing else: the new instance signals Wind_QuitRequest and we exit via the
    // normal quit path. (No self-terminate here - the handshake owns the teardown.)
}

// Guarantee EXACTLY ONE Wind.exe via a named mutex we OWN for our lifetime. This is the canonical,
// integrity-independent guard: the kernel auto-releases the mutex when its owner dies (even on a
// hard kill -> the next waiter gets WAIT_ABANDONED), so a zombie can NEVER permanently block a
// relaunch. `mtx` receives the owned handle. Returns false only when another LIVE instance will not
// yield - the caller then exits WITHOUT installing hooks, because two instances = two mouse hooks +
// two cursor-warp loops = a system-wide input lock. (The previous kill-only version failed here:
// TerminateProcess is blocked across the signed UIAccess build's integrity, and it had removed the
// refuse-to-start backstop, so a second instance proceeded anyway and locked input.)
static bool AcquireSingleInstance(HANDLE& mtx) {
    mtx = CreateMutexW(nullptr, FALSE, L"Local\\Wind_Magnifier_SingleInstance");
    SiLog("createmutex err", GetLastError());
    if (!mtx) { SiLog("mutex null - proceeding unprotected", 0); return true; }   // rare; don't block
    DWORD w = WaitForSingleObject(mtx, 0);   // WAIT_ABANDONED = prior owner died holding it -> ours now
    if (w == WAIT_OBJECT_0 || w == WAIT_ABANDONED) { SiLog("acquired immediately w", w); return true; }
    SiLog("busy - signaling quit, w", w);
    HANDLE ev = OpenEventW(EVENT_MODIFY_STATE, FALSE, L"Local\\Wind_QuitRequest");
    if (ev) { SetEvent(ev); CloseHandle(ev); SiLog("quit event set", 0); }
    else SiLog("quit event open err", GetLastError());
    w = WaitForSingleObject(mtx, 3000);                       // wait for it to release on clean exit
    if (w == WAIT_OBJECT_0 || w == WAIT_ABANDONED) { SiLog("acquired after quit w", w); return true; }
    SiLog("still busy after quit - terminating, w", w);
    TerminateOtherWind();                                     // fallback: kill the straggler
    w = WaitForSingleObject(mtx, 2000);
    if (w == WAIT_OBJECT_0 || w == WAIT_ABANDONED) { SiLog("acquired after terminate w", w); return true; }
    SiLog("REFUSING TO START - another instance alive, w", w);
    CloseHandle(mtx); mtx = nullptr;
    return false;                                             // never stack a second hook/cursor loop
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
    // Heal any input state a previous (possibly killed) Wind left dirty, then claim SOLE ownership.
    // If another live instance refuses to yield, exit WITHOUT installing hooks (two instances would
    // mean two mouse hooks + two cursor loops = input lock). atexit is the always-restore net for
    // CRT exit paths so no exit can leave the cursor hidden/confined.
    wind::LogInit(L"core");
    atexit(wind::LogShutdown);
    SiLog("=== launch ===", 0);
    LoadChurnyApps();   // issue #148: learned cursor-churning apps (transform -> render for them)
    DetectMpoDisabled();   // issue #148: MPO boot state decides whether the pan wall is needed
    RestoreInputState();
    HANDLE mtx = nullptr;
    if (!AcquireSingleInstance(mtx)) { RestoreInputState(); return 0; }
    atexit(AtExitRestore);
    // Installed before either magnifier model is constructed so a crash under model=transform (which
    // never touches RenderEngine, so RenderEngine's own filter is never installed) still heals a
    // blanked system cursor. See EarlyCursorRestoreFilter for why the render path safely replaces this.
    SetUnhandledExceptionFilter(EarlyCursorRestoreFilter);

    // Resolve magnifier.ini next to the exe (not the launch cwd).
    wchar_t exePath[MAX_PATH];
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH)) {
        wchar_t* slash = wcsrchr(exePath, L'\\');
        if (slash) { *slash = L'\0'; SetCurrentDirectoryW(exePath); }
    }

    // Resolve magnifier.ini's runtime path (exe-dir if writable, else %LOCALAPPDATA%\Wind\). Same
    // resolution is used by WindConfig.exe so both processes always touch the same file.
    std::wstring iniPath = wind::ResolveIniPath();
    Config cfg = LoadConfig(iniPath);

    // Render the live config as key=value lines for the snapshot.
    {
        std::ostringstream cd;
        cd << "maxLevel=" << cfg.maxLevel << "\nzoomInSpeed=" << cfg.zoomInSpeed
           << "\nzoomOutSpeed=" << cfg.zoomOutSpeed << "\nmultiMonitor=" << cfg.multiMonitor
           << "\ncropCapture=" << cfg.cropCapture << "\nvsync=" << cfg.vsync
           << "\ndwmFlush=" << cfg.dwmFlush << "\nzorderBand=" << cfg.zorderBand
           << "\ncursorVisibility=" << cfg.cursorVisibility << "\nhdrTonemap=" << cfg.hdrTonemap;
    #ifdef WIND_UIACCESS
        wind::LogSystemSnapshot("uiaccess", cd.str());
    #else
        wind::LogSystemSnapshot("normal", cd.str());
    #endif
    }

    // Hidden window: owns the tray icon + menu and receives WM_INPUT.
    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"WindMagnifierWnd";
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_WIND));  // logo badge for alt-tab/taskbar
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Wind", WS_OVERLAPPED,
                                0, 0, 0, 0, nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return 1;

    RAWINPUTDEVICE rid{};
    rid.usUsagePage = 0x01; rid.usUsage = 0x02; // generic mouse
    rid.dwFlags = RIDEV_INPUTSINK; rid.hwndTarget = hwnd;
    RegisterRawInputDevices(&rid, 1, sizeof(rid));

    // Safety: global Ctrl+Alt+Q quits cleanly from anywhere (works even with the overlay up
    // and the cursor hidden). If the combo is already taken, the tray Quit still works.
    RegisterHotKey(hwnd, kQuitHotkeyId, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'Q');
    RegisterHideCursorHotkey(hwnd, cfg.hideCursorVk, cfg.hideCursorMods);
    RegisterQuickZoomHotkey(hwnd, (cfg.quickZoomHotkeyMode && cfg.quickZoomVk) ? cfg.quickZoomVk : 0,
                            cfg.quickZoomMods);

    if (!g_input.start(cfg.zoomInButton, cfg.zoomInButton2, cfg.zoomOutButton, cfg.zoomOutButton2,
                       /*swallow=*/true)) {
        MessageBoxW(nullptr, L"Failed to install the mouse hook.", L"Wind", MB_ICONERROR);
        return 1;
    }
    // Configure the keyboard hook's bound keys (zoom in/out primary+alt, recenter, Inspect-mode
    // cursor-lock, and magnifier-model swap) so it swallows them and tracks their state. Kept in
    // sync on hot-reload below.
    g_input.setKeys(cfg.zoomInVk, cfg.zoomInVk2, cfg.zoomOutVk, cfg.zoomOutVk2, cfg.recenterVk,
                    cfg.cursorLockVk, cfg.swapModelVk);

    // Target monitor for this session: the cursor's monitor when multiMonitor is on, else the
    // primary. The first zoom-in re-checks and retargets if the cursor moved to another monitor.
    // The magnify model has no overlay of its own (Windows Magnifier owns the view), so monitor
    // targeting is a documented no-op there; it just gets the primary.
    MonitorTarget startupMon = (cfg.model == "render" && cfg.multiMonitor != 0)
                                   ? MonitorUnderCursor() : PrimaryMonitor();

    // --- Magnifier model (render: DXGI Desktop Duplication + D3D11 overlay; magnify: drive the
    // native Windows Magnifier via injected Win+Plus/Minus, the DRM-safe fallback) ---
    std::unique_ptr<IMagnifierModel> model;       // primary engine (also the hybrid's render half)
    std::unique_ptr<IMagnifierModel> model2;      // hybrid only: the transform half
    if (cfg.model == "magnify") {
        model = std::make_unique<MagnifyModel>();
        // Our injected chords must never be swallowed/tracked by our own keyboard hook
        // (NumPad +/- are bindable zoom keys; see InputRouter::setIgnoreInjectedKeys).
        g_input.setIgnoreInjectedKeys(true);
    } else if (cfg.model == "transform") {
        // Revived for issue #148: the DWM-internal fullscreen transform - zero app presents, so
        // it holds compositor-rate smoothness over a heavy game where every overlay present path
        // throttles (measured). Cursor is anchored, not centered (documented model tradeoff).
        model = std::make_unique<TransformModel>(cfg.fastPan != 0, cfg.smoothPan != 0,
                                                 cfg.cursorSprite != 0, cfg.zorderBand);
    } else {
        model = std::make_unique<RenderModel>(cfg.zorderBand, cfg.hdrTonemap != 0,
                                              EffectiveGpuPriority(cfg));
        if (cfg.model == "hybrid") {
            // Hybrid (issue #148): render model on the desktop (centered cursor, unlimited
            // levels), transform model whenever a fullscreen app is foreground at zoom-in
            // (compositor-internal - the only path that stays smooth over a heavy game). The
            // engine is picked per zoom-in session in RunTick; both stay initialized.
            model2 = std::make_unique<TransformModel>(cfg.fastPan != 0, cfg.smoothPan != 0,
                                                      cfg.cursorSprite != 0, cfg.zorderBand);
        }
    }
    if (!model->initialize(startupMon)) {
        MessageBoxW(nullptr, L"Could not start the renderer (Direct3D 11 / Desktop Duplication "
                             L"unavailable on this system).", L"Wind", MB_ICONERROR);
        g_input.stop();
        return 1;
    }
    if (model2 && !model2->initialize(PrimaryMonitor())) {
        wind::Log(wind::LogLevel::Warn, "startup", "hybrid: transform half failed to init; render-only");
        model2.reset();
    }

    Tray::Add(hwnd, hInst);

    TickState ts(model.get(), startupMon, cfg);
    ts.mRender = model.get();
    ts.mTransform = model2.get();
    ts.hwnd = hwnd;                       // so RunTick can re-register the hide-cursor hotkey
    g_tick = &ts;   // so the WM_TIMER tick (during the tray menu's modal loop) can run

    // Autonomous verification hook: WIND_SELFTEST drives the real integrated render path at a
    // forced zoom and dumps a PNG (the overlay is WDA_EXCLUDEFROMCAPTURE, so it can only be
    // captured from inside the app), then exits. Not part of normal use.
    if (GetEnvironmentVariableW(L"WIND_SELFTEST", nullptr, 0) > 0) {
        // Selftest drives the render path directly, so it only runs for the RenderModel.
        if (auto* rm = dynamic_cast<RenderModel*>(model.get())) {
            RenderEngine& renderEngine = rm->engine();
            POINT pt; GetCursorPos(&pt);
            ts.mapper.reset(pt.x - ts.mon.x, pt.y - ts.mon.y);
            renderEngine.hideSystemCursor(true);
            renderEngine.setVisible(true);
            RenderFrameParams p{};
            for (int i = 0; i < 20; ++i) {
                MSG m; while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
                MapResult r = ts.mapper.update(0, 0, 4.0);
                FillRenderParams(p, r, cfg, ts.mon, 4.0);
                p.cursorMode = 1;   // always draw the cursor in the selftest dump
                p.vsync = true;
                renderEngine.renderFrame(p);
                Sleep(16);
            }
            renderEngine.dumpFrame(p, L"wind_selftest.png");
            unsigned ddaFmt = 0; int cs = -1, bpc = 0; renderEngine.debugHdr(ddaFmt, cs, bpc);
            FILE* hf = nullptr; _wfopen_s(&hf, L"wind_hdr_diag.txt", L"w");
            if (hf) { fprintf(hf, "ddaFormat=%u outColorSpace=%d bitsPerColor=%d\n", ddaFmt, cs, bpc); fclose(hf); }
            renderEngine.shutdown();
        }
        g_input.stop();
        Tray::Remove();
        ReleaseMutex(mtx);
        return 0;
    }

    // Frame-pacing self-test: WIND_PACINGTEST runs the REAL present-paced render path at a forced
    // zoom with a simulated pan for ~4 s and logs loop-interval stats to %TEMP%\wind_diag.log -
    // to measure microstutter objectively (the normal loop needs the side button to zoom). Exits.
    if (GetEnvironmentVariableW(L"WIND_PACINGTEST", nullptr, 0) > 0) {
        // Pacing test drives the render path directly, so it only runs for the RenderModel.
        if (auto* rm = dynamic_cast<RenderModel*>(model.get())) {
            RenderEngine& renderEngine = rm->engine();
            POINT pt; GetCursorPos(&pt);
            ts.mapper.reset(pt.x - ts.mon.x, pt.y - ts.mon.y);
            renderEngine.hideSystemCursor(true);
            LARGE_INTEGER f, a{}, b; QueryPerformanceFrequency(&f);
            const double target = 1.0 / DetectRefreshHz();
            double elapsed = 0.0, sumDt = 0.0, maxDt = 0.0; int frames = 0, hitches = 0, big = 0;
            bool first = true;
            QueryPerformanceCounter(&a);
            while (elapsed < 4.0) {
                MSG m; while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
                int dxp = ((frames / 20) % 2 == 0) ? 6 : -6;   // oscillate the pan so srcRect keeps moving
                MapResult r = ts.mapper.update(dxp, 0, 4.0);
                RenderFrameParams p{};
                FillRenderParams(p, r, cfg, ts.mon, 4.0);
                p.cursorMode = 1; p.vsync = (cfg.vsync != 0);
                if (first) renderEngine.invalidateCapture();
                renderEngine.renderFrame(p);
                if (first) { renderEngine.setVisible(true); first = false; QueryPerformanceCounter(&a); continue; }
                QueryPerformanceCounter(&b);
                double dt = double(b.QuadPart - a.QuadPart) / f.QuadPart; a = b;
                elapsed += dt; sumDt += dt; ++frames;
                if (dt > maxDt) maxDt = dt;
                if (dt > target * 1.5) ++hitches;
                if (dt > target * 2.5) ++big;
            }
            DiagLog("PACINGTEST vsync=%d frames=%d ~fps=%.1f targetDt=%.2fms avgDt=%.2fms maxDt=%.2fms hitches>1.5x=%d big>2.5x=%d",
                    cfg.vsync, frames, frames / elapsed, target * 1000.0,
                    (frames ? sumDt / frames : 0.0) * 1000.0, maxDt * 1000.0, hitches, big);
            renderEngine.shutdown();
        }
        g_input.stop();
        Tray::Remove();
        ReleaseMutex(mtx);
        return 0;
    }

    // First launch: open the guided setup once (at startup, before the tick loop). onboarded==0
    // covers a freshly created ini too. Non-blocking: spawn WindConfig.exe --onboard, then continue
    // to the tray. Resolve by full path (exePath is our own dir) so it works regardless of the cwd.
    if (cfg.onboarded == 0) {
        std::wstring configExe = std::wstring(exePath) + L"\\WindConfig.exe";
        wchar_t cmd[] = L"WindConfig.exe --onboard";
        STARTUPINFOW si{}; si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        if (CreateProcessW(configExe.c_str(), cmd, nullptr, nullptr, FALSE,
                           0, nullptr, nullptr, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
    }

    QueryPerformanceFrequency(&ts.freq);
    QueryPerformanceCounter(&ts.prev);
    ts.iniPath = iniPath;
    ts.lastMtime = ConfigMTime(iniPath);
    // Watch the directory holding the ini so config hot-reload doesn't stat magnifier.ini every
    // second on the render thread (see RunTick). LAST_WRITE catches in-place saves; FILE_NAME
    // catches write-temp-then-rename saves. nullptr/INVALID on failure -> RunTick falls back to
    // the timed poll. Watched dir is iniPath's parent (exe dir for dev, %LOCALAPPDATA%\Wind for
    // a Program Files install).
    std::wstring iniDir = iniPath.substr(0, iniPath.find_last_of(L"\\/"));
    ts.configWatch = FindFirstChangeNotificationW(iniDir.c_str(), FALSE,
        FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME);

    // Quit-request channel for WindConfig.exe (onboarding close). A window message can't be used:
    // the deployed Wind.exe is UIAccess, and UIPI silently blocks PostMessage from the non-UIAccess
    // WindConfig. A named event is a kernel object (not gated by UIPI) and both run as the same user
    // in the same session, so it works in dev and deployed. Auto-reset, initially unsignaled.
    HANDLE quitEvent = CreateEventW(nullptr, FALSE, FALSE, L"Local\\Wind_QuitRequest");

    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr,
        CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    // Auto-detect the display refresh rate so we never assume a fixed rate (the dev's 144Hz).
    // Paces the idle/1x loop and the vsync=0 path; while zoomed, DwmFlush/vsync pace instead.
    ts.hz = DetectRefreshHz();
    int pacedHz = ts.hz;                              // hz the timer interval below is computed for
    LARGE_INTEGER due; due.QuadPart = -(10000000LL / pacedHz);

    bool running = true;
    unsigned long long nextRecoverMs = 0;   // device-lost recovery backoff gate (GetTickCount64)
    // The transform model does no blocking present, so it can never self-pace via Present(1,0) or
    // DwmFlush the way the render model does. It must always be timer-paced (like the idle/1x path),
    // or the zoomed loop spins flat out and floods MagSetFullscreenTransform, backing up DWM's
    // desktop-transform queue so the view lags ~1-2s behind input. Cache the model kind once.
    // hybrid swaps engines per zoom-in: current-engine check is per-iteration in the loop
    while (running) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { running = false; break; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!running) break;
        // External quit request (WindConfig onboarding close). Break to the clean shutdown below
        // (restores cursor, resets zoom, removes the tray icon).
        if (quitEvent && WaitForSingleObject(quitEvent, 0) == WAIT_OBJECT_0) { running = false; break; }

        // Device-lost recovery (GPU TDR, driver update, adapter change). renderFrame() reported the
        // D3D device was removed; rebuild it on a backoff so we don't spin (the driver may take a
        // moment to return). Crucially, un-hide the OS cursor first so the user is never left without
        // a pointer while we are unable to draw the magnified one. Skip the normal tick this iteration.
        if (auto* rm = dynamic_cast<RenderModel*>(ts.mRender); rm && rm->deviceLost()) {
            // TDR backstop (issue #148): if a transform GAME session was live within the last
            // 30s, this device-lost almost certainly IS the driver reset that session caused
            // (e.g. an animated cursor churning invisibly to the handle poll). Remember the app
            // so it never gets the transform path again - one crash ever, then render.
            if (!ts.freezeExe.empty() &&
                GetTickCount64() - ts.lastFreezeActiveMs < 30000) {
                MarkChurnyApp(ts.freezeExe, "device-lost backstop");
            }
            rm->hideSystemCursor(false);   // restore the real cursor while we can't render
            // Inspect's 1px freeze clip must not survive a device-lost: release it and clear the toggle so
            // the post-recovery tick can't re-clip the cursor to the stale frozen pixel (honors the
            // documented "released on device-lost recovery" invariant; recovery returns to a clean 1x).
            ClipCursor(nullptr);
            ts.gameFreeze = false;   // the game-session freeze clip is the same invariant
            EndFreezeSteal(ts);      // tdrTest=3 must not strand the game backgrounded either
            if (ts.restAfterReveal) { ts.restAfterReveal->setActive(false); ts.restAfterReveal = nullptr; }
            if (ts.cursorLock.locked()) { ts.cursorLock.reset(); ts.clickReleaseTicks = 0; }
            EndGameInspect(ts);   // device-lost must not strand the game backgrounded
            unsigned long long now = GetTickCount64();
            if (now >= nextRecoverMs) {
                if (!rm->recoverDeviceLost()) nextRecoverMs = now + 500;   // retry in 0.5s
                else { ts.prevLvl = 1.0; ts.zoom = ZoomController(1.0, ts.cfg.maxLevel); }  // back to 1x, clean
            }
            Sleep(50);
            continue;
        }

        // Pacing while zoomed:
        //  - dwmFlush: present immediately, then DwmFlush() AFTER the tick aligns us 1:1 with the
        //    compositor (targets blt-model microstutter). No pre-tick wait.
        //  - vsync: Present(1,0) blocks to the refresh and paces the loop (skip the timer to
        //    avoid timer/vsync double-pacing).
        //  - else: Present(0,0) doesn't block, so the timer paces at the detected refresh rate.
        // Idle at 1x uses the timer.
        bool zoomed = ts.prevLvl > 1.0;
        const bool renderModelActive = dynamic_cast<RenderModel*>(ts.model) != nullptr;
        // dwmFlush=1 -> present immediately then DwmFlush (align 1:1 with the compositor, targets the
        // blt-model microstutter); else vsync=1 -> Present(1,0) blocks; else the timer paces.
        // The render model keeps its configurable self-pacing (blocking Present / DwmFlush). The
        // transform model submits via MagSetFullscreenTransform (no blocking present), so DwmFlush is
        // its ONLY coherent pace while zoomed: it blocks one composite per tick so the sprite update
        // and the transform land in the SAME frame. A plain timer lets them drift into different
        // composites, so the cursor beats against the panning view (the flicker) - exactly what
        // DwmFlush prevents (and it paces at refresh, so no flood either). Bloom paces this way too.
        bool dwmPaces = zoomed && (renderModelActive ? (ts.cfg.dwmFlush != 0) : true);
        // Game pacing engaged: presents are non-blocking Present(0,0) frames (and may be skipped
        // by the fence gate), so the blocking-present pace is unavailable - fall through to the
        // timer (full tick rate; frame work skips inside renderFrame as needed).
        // The reduced-push game mode (gameFpsCap with vsync) paces itself inside RunTick
        // (Present(1,0) on present ticks, WaitForVBlank on skip ticks), so it counts as
        // present-paced here and must NOT also wait on the timer.
        bool renderPresentPaces = renderModelActive && zoomed && !dwmPaces && ts.cfg.vsync != 0 &&
                                  !ts.gamePacing;
        if (!renderPresentPaces && !dwmPaces) {
            // Recompute the timer interval if the paced refresh changed (retarget to a different-Hz
            // monitor updates ts.hz). Cheap equality check; only recomputes on an actual change (#74).
            if (ts.hz > 0 && ts.hz != pacedHz) { pacedHz = ts.hz; due.QuadPart = -(10000000LL / pacedHz); }
            if (timer) {
                SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
                WaitForSingleObject(timer, INFINITE);
            } else {
                Sleep(1000 / pacedHz);
            }
        }

        RunTick(ts);

        if (dwmPaces) DwmFlush();   // block until DWM's next composite -> frames align with it
    }

    g_tick = nullptr;
    if (timer) CloseHandle(timer);
    if (quitEvent) CloseHandle(quitEvent);
    if (ts.configWatch && ts.configWatch != INVALID_HANDLE_VALUE) FindCloseChangeNotification(ts.configWatch);
    UnregisterHotKey(hwnd, kQuitHotkeyId);
    UnregisterHotKey(hwnd, kHideCursorHotkeyId);
    UnregisterHotKey(hwnd, kQuickZoomHotkeyId);
    EndGameInspect(ts);  // quitting mid-game-inspect hands foreground back to the game
    model->shutdown();   // restores cursor + tears down D3D/overlay
    g_input.stop();
    Tray::Remove();
    if (mtx) { ReleaseMutex(mtx); CloseHandle(mtx); }
    wind::LogShutdown();
    return 0;
}
