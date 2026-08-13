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
#include "profiles_io.h"
#include "drag_follow.h"
#include "engine_pick.h"
#include "mpo_boot.h"
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
#include "shell_desktop.h"
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
    // Persist the earliest post-boot reading so the Settings "Disable MPO" row can tell a change
    // that genuinely needs a restart from one that merely puts the registry back to what DWM
    // already loaded (issue #164). No-ops if this boot is already recorded.
    wind::RecordMpoBootState(g_mpoDisabled);
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
    POINT          lastSetVirtual{};  // MEASURED post-present pointer position (virtual px), the
                                      // baseline for the next tick's hand delta (issue #169: never
                                      // assume the weld landed - measure)
    // Divergence diagnostics (issue #169; diagnostics=1 only): 1 Hz aggregate while zoomed.
    double             dbgMaxDivergence = 0.0;
    unsigned           dbgDragFollowTicks = 0;
    unsigned long long dbgDivergenceLogMs = 0;
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
    int    clickPauseTicks = 0;     // ticks to skip transform writes around an Inspect click's
                                    //   injected absolute move (a write racing it is the TDR class)
    unsigned long long quiesceUntilMs = 0;  // launch quiesce (dwmcore APPCRASH, RDR2 @20x,
                                    //   2026-08-12): hold ALL transform mutations (writes, input
                                    //   transform, weld) while the compositor digests a LAUNCHING
                                    //   game's takeover. Deadline anchored at cover SIGHT time,
                                    //   never at zoom-in (see TrackLaunchCover).
    DWORD  quiescedPid = 0;         // quiesce fires at most once per process instance
    HWND   lastCoverFg = nullptr;   // edge-detect cover-takeover foregrounds
    unsigned long long lastCoverProbeMs = 0;   // throttles the idle-tick cover watch to ~4Hz
    IMagnifierModel* wantModel = nullptr;   // hybrid stickiness: candidate engine and how long it
    unsigned long long wantSinceMs = 0;     //   has been the candidate (debounces foreground reads)
    unsigned long long kbHookDivergentSinceMs = 0;  // LL keyboard-hook watchdog dwell (issue #156)
    unsigned long long lastFgProbeMs = 0;           // throttles the game-foreground probe to ~10Hz
    std::wstring transformExe;      // exe of the app under the current/last transform game session
    unsigned long long lastTransformGameMs = 0;  // TDR-backstop window (device-lost attribution)
    // Per-HWND cache for the exe-derived pick predicates (shell class, exclusion list, churny
    // list). The instant re-pick runs every zoomed tick; opening the foreground process and
    // building exe-name strings 144x/s answered a question that only changes when the foreground
    // WINDOW changes. covers/borderless stay per-tick (cheap user32 reads, genuinely dynamic).
    HWND fgCacheHwnd = nullptr;
    bool fgCacheShell = false, fgCacheExcluded = false, fgCacheChurny = false;
    bool probePrevLDown = false;    // dead-zone probe (probeClicks=1): left-click edge detect
    unsigned probeTraceTick = 0;    // dead-zone probe (probeClicks=2): trace decimation counter
    bool   inspectCursorWasShowing = true; // cursor visibility at the toggle edge (the mouselook tell)
    bool   cursorHiddenByUs = false;      // WE currently hide the OS cursor (render zoom / Inspect).
                                          //   A transform FOLLOW session leaves it alone, so the app's
                                          //   own hiding stays readable - see ShouldGameInspect.
    bool   inspectMagHidCursor = false;   // snapshot of the above at the Inspect toggle edge
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

// Is the foreground window's exe named in a comma-separated list? Shared by every exe-list check
// (transformExclude, noSwallowApps, the built-in overlay names) so they all match identically:
// bare file name, case-insensitive, exact - never a path.
static bool FgExeInList(HWND fg, const std::string& list) {
    if (list.empty()) return false;
    const std::wstring exeW = ExeNameOf(fg);
    if (exeW.empty()) return false;
    std::string exe;
    exe.reserve(exeW.size());
    for (wchar_t ch : exeW) exe.push_back((char)(ch < 128 ? ch : '?'));
    return wind::IsExeInList(exe, list);
}
// Auto/hybrid exclusion: an app on cfg.transformExclude never gets the transform engine even when
// it is fullscreen and borderless. Fullscreen browser video is indistinguishable from a game by
// the foreground test, but it wants render (constant-size cursor, desktop-style behaviour).
static bool IsTransformExcluded(HWND fg, const Config& cfg) {
    return FgExeInList(fg, cfg.transformExclude);
}
// Overlays that cover the screen, look exactly like a game to the foreground test, and are gone
// again in a couple of seconds. Hard-coded on purpose: these are system surfaces, not something a
// user can sensibly pick out of a file browser, and a second user-managed list is not worth the
// settings surface. Only consulted for windows that already failed the free style test below.
static const char* kOverlayExes =
    "SnippingTool.exe,ScreenSketch.exe,ScreenClippingHost.exe,TextInputHost.exe";

// Is the foreground an overlay rather than a real app? While one is up the Auto engine choice is
// FROZEN: such a tool holds foreground for a couple of seconds and hands it straight back, so
// re-picking on it costs TWO engine handovers within seconds, and each one releases and rebuilds
// DWM's magnification context - a pair of stalls exactly when the user is trying to read the
// screen. The 350ms stickiness cannot help; these are visible for far longer than that.
//
// Deliberately cheap: no hook, no polling, nothing that loops.
//   1. WS_EX_LAYERED on the foreground window - one GetWindowLongPtr, right next to the GWL_STYLE
//      read the caller already makes. A game does not use a layered top-level window (it would
//      cost it the redirection surface and its independent-flip plane); capture tools, dimmers and
//      click-through HUDs do. This alone catches most of them for free.
//   2. Only if that misses, the small built-in name list, which needs a process handle.
// Memoised on the HWND so step 2 costs one lookup per foreground CHANGE rather than one per tick.
// Shell surfaces that grab foreground for a moment and hand it straight back: the taskbar and
// its preview/tray flyouts (issue #180 - clicking a taskbar preview button over a transform
// session ping-ponged the engine, and every handover shows the over-zoom pulse), the Win10
// thumbnail flyout, and the Start/search CoreWindow. Matched by CLASS, not exe: these all live
// in explorer.exe, and excluding all of explorer would swallow real File Explorer windows.
static bool IsShellTransientClass(const wchar_t* cls) {
    return lstrcmpiW(cls, L"Shell_TrayWnd") == 0 ||
           lstrcmpiW(cls, L"Shell_SecondaryTrayWnd") == 0 ||
           lstrcmpiW(cls, L"TaskListThumbnailWnd") == 0 ||                 // Win10 preview flyout
           lstrcmpiW(cls, L"XamlExplorerHostIslandWindow") == 0 ||        // Win11 flyouts/alt-tab
           lstrcmpiW(cls, L"TopLevelWindowForOverflowXamlIsland") == 0 || // Win11 tray overflow
           lstrcmpiW(cls, L"Windows.UI.Core.CoreWindow") == 0;            // Start menu / search
}

static bool IsOverlayFg(HWND fg) {
    if (!fg) return false;
    if (GetWindowLongPtrW(fg, GWL_EXSTYLE) & WS_EX_LAYERED) return true;
    static HWND s_lastFg = nullptr;
    static bool s_lastResult = false;
    if (fg == s_lastFg) return s_lastResult;
    s_lastFg = fg;
    wchar_t cls[64]{};   // longest listed class is 35 chars; longer real classes truncate + miss
    GetClassNameW(fg, cls, 64);
    s_lastResult = IsShellTransientClass(cls) || FgExeInList(fg, kOverlayExes);
    return s_lastResult;
}

// Shell desktop (issue #172): after Win+D / "show desktop", foreground goes to Progman (or a
// WorkerW when a live-wallpaper tool has re-parented SHELLDLL_DefView) - a caption-less window
// covering the whole monitor, indistinguishable from a game by the style test alone. The desktop
// always wants render, so both engine picks exclude it. Class test, not exe: excluding all of
// explorer.exe would be broader than needed, and the class also covers the Wallpaper Engine
// WorkerW case where the foreground window is not explorer's.
static bool IsShellDesktopFg(HWND fg) {
    if (!fg) return false;
    char cls[16]{};   // "Progman"/"WorkerW" fit; a longer class truncates and simply won't match
    GetClassNameA(fg, cls, sizeof(cls));
    return wind::IsShellDesktopClass(cls);
}

// Keyboard-hook suspension list (issue #156): while one of these apps is foreground the LL keyboard
// hook is uninstalled, so Windows' input thread stops round-tripping every keystroke through us and
// the mouse stream to that app is never stalled. Unlike the transform exclusion this does NOT
// require fullscreen: the user named the app, so honour it whenever it is in front.
static bool IsNoSwallowApp(HWND fg, const Config& cfg) {
    return FgExeInList(fg, cfg.noSwallowApps);
}

// Every cursor hide/show the tick loop performs goes through here, so cursorHiddenByUs is always an
// exact record of whether WE are the reason the OS cursor is invisible. game-inspect needs that: a
// hidden cursor is the mouselook tell, but only when we did not hide it ourselves (ShouldGameInspect).
static void SetSystemCursorHidden(TickState& t, IMagnifierModel* m, bool hide) {
    if (!m) return;
    m->hideSystemCursor(hide);
    t.cursorHiddenByUs = hide;
}

// Whether the foreground window's PROCESS started within the last `ms` milliseconds - the tell
// for a game LAUNCH taking over (vs an alt-tab into a long-running one).
static bool FgProcessYoungerThanMs(HWND fg, unsigned long long ms) {
    DWORD pid = 0;
    if (!fg || !GetWindowThreadProcessId(fg, &pid) || !pid) return false;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;
    FILETIME created{}, exit_{}, kern{}, user{};
    bool young = false;
    if (GetProcessTimes(h, &created, &exit_, &kern, &user)) {
        FILETIME nowFt{}; GetSystemTimeAsFileTime(&nowFt);
        ULARGE_INTEGER c{ created.dwLowDateTime, created.dwHighDateTime };
        ULARGE_INTEGER n{ nowFt.dwLowDateTime, nowFt.dwHighDateTime };
        young = n.QuadPart > c.QuadPart && (n.QuadPart - c.QuadPart) / 10000ULL < ms;
    }
    CloseHandle(h);
    return young;
}

// Launch-quiesce cover tracking (dwmcore APPCRASH, 2026-08-12; re-scoped 2026-08-13 after the
// field review): the ~1.5s transform-write hold exists to sit out a LAUNCHING game's mode-switch/
// surface churn. The clock is anchored when the young cover is FIRST SEEN (idle ticks probe at
// ~4Hz, active ticks call per tick) - NOT at zoom-in: the old zoom-in-anchored hold started its
// full window exactly when the user pressed zoom, eating the whole first press (log-proven twice,
// the "first zoom key press does nothing, let go and hold again" report). Armed at most once per
// process instance, so re-entering the game from the desktop inside its first 60s (or the game
// re-creating its cover window) never re-pays the hold. In the common case - zooming a few
// seconds after the game window appears - the window has already burned down and the hold is zero.
static void TrackLaunchCover(TickState& t, HWND fg, bool fsCover, bool fgBorderless) {
    if (fsCover && fgBorderless) {
        if (fg != t.lastCoverFg) {
            t.lastCoverFg = fg;
            DWORD pid = 0;
            GetWindowThreadProcessId(fg, &pid);
            if (pid && pid != t.quiescedPid && FgProcessYoungerThanMs(fg, 60000)) {
                t.quiescedPid = pid;
                t.quiesceUntilMs = GetTickCount64() + 1500;
                wind::Log(wind::LogLevel::Info, "transform",
                          "launch quiesce: fresh cover %ls - transform writes held ~1.5s from sighting",
                          ExeNameOf(fg).c_str());
            }
        }
    } else {
        t.lastCoverFg = nullptr;
    }
}

// Whether the launch-quiesce hold is live THIS tick for the current model. Only the transform
// model pauses (the hold protects DWM's magnification path; render/magnify are unaffected).
static bool QuiesceHoldActive(const TickState& t) {
    return t.quiesceUntilMs != 0 && GetTickCount64() < t.quiesceUntilMs &&
           dynamic_cast<TransformModel*>(t.model) != nullptr;
}

// Refresh the per-HWND cache of exe-derived pick predicates. Only re-resolves when the foreground
// WINDOW changed; a null fgw clears to safe defaults (no transform pick without a foreground).
static void RefreshFgCache(TickState& t, HWND fgw) {
    if (fgw == t.fgCacheHwnd) return;
    t.fgCacheHwnd = fgw;
    t.fgCacheShell    = IsShellDesktopFg(fgw);
    t.fgCacheExcluded = IsTransformExcluded(fgw, t.cfg);
    t.fgCacheChurny   = IsChurnyFg(fgw);
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
             || nc.recenterVk != t.cfg.recenterVk || nc.cursorLockVk != t.cfg.cursorLockVk) {
                g_input.setKeys(nc.zoomInVk, nc.zoomInVk2, nc.zoomOutVk, nc.zoomOutVk2, nc.recenterVk,
                                nc.cursorLockVk);
            }
            if (nc.hideCursorVk != t.cfg.hideCursorVk || nc.hideCursorMods != t.cfg.hideCursorMods) {
                RegisterHideCursorHotkey(t.hwnd, nc.hideCursorVk, nc.hideCursorMods);
            }
            if (nc.quickZoomHotkeyMode != t.cfg.quickZoomHotkeyMode
             || nc.quickZoomVk != t.cfg.quickZoomVk || nc.quickZoomMods != t.cfg.quickZoomMods) {
                RegisterQuickZoomHotkey(t.hwnd, (nc.quickZoomHotkeyMode && nc.quickZoomVk) ? nc.quickZoomVk : 0,
                                        nc.quickZoomMods);
            }
            // txIdleReleaseMs is documented hot-reloadable; push it into whichever transform
            // model exists (pure-transform t.model or hybrid's t.mTransform - never both).
            if (auto* tmHot = dynamic_cast<TransformModel*>(
                    t.mTransform ? t.mTransform : t.model))
                tmHot->setIdleReleaseMs(nc.txIdleReleaseMs);
            t.cfg = nc;   // pick up renderer knobs (smoothing, filter, cursor scale, zoom speed)
            t.fgCacheHwnd = nullptr;   // transformExclude may have changed: re-resolve predicates
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
    // Suspend the LL keyboard hook while a borderless fullscreen app (a game) is foreground.
    //
    // A WH_KEYBOARD_LL hook taxes the SYSTEM's input pipeline, not just ours: the raw input thread
    // dispatches every keystroke to the hooking thread and waits for it to return before delivering
    // any further input, INCLUDING mouse movement to the foreground game. Holding a key in a game
    // (auto-repeat, ~30/s) therefore punches a stall into the mouse stream on every repeat - the
    // "panning is smooth until I hold a key" stutter. The cost is the hook's EXISTENCE: an unbound
    // key like Ctrl stalls identically, swallowing is irrelevant, and the stutter disappeared
    // completely in the field whenever Windows had evicted the hook (and returned the instant the
    // watchdog healed it). It is also why the native Windows Magnifier shows the same stutter.
    //
    // Swallowing buys nothing in a game anyway: an LL hook cannot block raw input, which is what
    // games read (documented limitation above), so the hook is pure cost there. Suspend it over a
    // fullscreen borderless foreground and restore it on the desktop, where swallowing does work.
    // Binds keep working while suspended - nothing swallows them, so keyDown's GetAsyncKeyState
    // fallback reads them correctly. Same borderless-cover test the hybrid model uses to spot games.
    // Throttled to ~10 Hz: foreground changes are human-speed events, so probing them every tick
    // (display refresh) would burn a few window queries 144x a second to answer a question that
    // changes maybe once a minute. Worst case the swap lands 100 ms late, which nobody can feel.
    // OPT-IN, off by default: unconfigured, the hook stays installed and keys are swallowed
    // everywhere exactly as before, and this costs a single string check per tick (no window
    // queries at all). Configure noSwallowApps (per-app, the only knob) to trade swallowing for
    // smooth panning in that app.
    {   // Launch-quiesce cover watch (see TrackLaunchCover): probe at ~4Hz so a young game cover
        // is SIGHTED while the user is still idle and the 1.5s hold burns down before the first
        // zoom-in. The active block below re-tracks per tick with its already-read facts.
        const unsigned long long nowMs = GetTickCount64();
        if (nowMs - t.lastCoverProbeMs >= 250) {
            t.lastCoverProbeMs = nowMs;
            HWND fg = GetForegroundWindow();
            const bool cover = ForegroundCoversMonitor(t.mon);
            const bool borderless = fg && !(GetWindowLongPtrW(fg, GWL_STYLE) & WS_CAPTION);
            TrackLaunchCover(t, fg, cover, borderless);
        }
    }
    if (t.cfg.noSwallowApps.empty()) {
        g_input.setKeyboardHookWanted(true);   // idempotent: only posts on an actual change
    } else {
        const unsigned long long nowMs = GetTickCount64();
        if (nowMs - t.lastFgProbeMs >= 100) {
            t.lastFgProbeMs = nowMs;
            // Does NOT require fullscreen: the user named the app, so honour it whenever that app
            // is in front (windowed play, borderless, either way).
            g_input.setKeyboardHookWanted(!IsNoSwallowApp(GetForegroundWindow(), t.cfg));
        }
    }
    // LL keyboard-hook watchdog (issue #156). Windows SILENTLY evicts a low-level hook whose
    // callback misses LowLevelHooksTimeout: no notification, no error, the handle stays valid and
    // KbProc simply never fires again. A game's launch load spike is exactly when that happens -
    // launching a heavily modded RDR2 with Wind already running killed every keyboard bind while
    // the mouse binds survived, and rebinding a key then looked like a broken ini hot-reload (the
    // reload DID apply; the dead hook just never reported the key, and kbHookActive() still claimed
    // the hook was the authority, so keyDown below asked it and always got "up").
    //
    // The tell needs no extra bookkeeping: while the hook is alive it SWALLOWS every bound key, so
    // GetAsyncKeyState can NEVER see one. Poller sees a bound key held + hook still reports it up
    // => the hook is gone. The dwell keeps the ordinary press-before-callback race from
    // false-positiving; the magnify model is excluded outright because its hook deliberately skips
    // the injected chords it drives Windows Magnifier with (those are unswallowed by design).
    constexpr unsigned long long kKbHookDeadMs = 250;
    if (g_input.kbHookActive() && g_input.swallowEnabled() && !g_input.ignoreInjectedKeys()) {
        const int watched[] = { t.cfg.zoomInVk, t.cfg.zoomInVk2, t.cfg.zoomOutVk, t.cfg.zoomOutVk2,
                                t.cfg.recenterVk, t.cfg.cursorLockVk };
        bool divergent = false;
        for (int vk : watched) {
            if (vk == 0 || !g_input.isBoundKey(vk)) continue;
            if ((GetAsyncKeyState(vk) & 0x8000) && !g_input.keyPressed(vk)) { divergent = true; break; }
        }
        const unsigned long long nowMs = GetTickCount64();
        if (!divergent)                             t.kbHookDivergentSinceMs = 0;
        else if (t.kbHookDivergentSinceMs == 0)     t.kbHookDivergentSinceMs = nowMs;
        else if (nowMs - t.kbHookDivergentSinceMs >= kKbHookDeadMs) {
            t.kbHookDivergentSinceMs = 0;
            g_input.requestKbHookReinstall();   // logs, and polling takes over on the next tick
        }
    } else {
        t.kbHookDivergentSinceMs = 0;
    }
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
    // Launch-quiesce ramp freeze (field review, 2026-08-13): while the hold pauses all transform
    // writes, the controller must NOT keep integrating - the silently accumulated level would land
    // as ONE giant discrete write at expiry (the measured 30-50ms game-frame class, aimed at a
    // game that is by definition still launching; log-proven 1.0 -> 4.5 in one write). Freezing
    // the tick instead makes the ramp START when writes resume: a smooth ramp, merely delayed.
    // Gated on level > 1.0 so the session still ENTERS (one tick of ramp) and the freeze holds it
    // at ~1.01 until the hold expires; a zoom-out pressed inside the window freezes too,
    // consistent with the freeze-the-lens intent.
    const bool quiesceFreeze = QuiesceHoldActive(t) && t.zoom.level() > 1.0;
    if (!quiesceFreeze) t.zoom.tick(dt < kMaxZoomDt ? dt : kMaxZoomDt);
    // Recenter on a recenterVk key press (rising edge).
    bool recenter = false;
    bool recenterDown = keyDown(t.cfg.recenterVk);
    if (recenterDown && !t.recenterKeyWasDown) recenter = true;
    t.recenterKeyWasDown = recenterDown;
    // Inspect mode: toggle on the bound key's rising edge (works at any zoom). The crosshair is
    // overlay-drawn (render_engine draws the crosshair sprite when cursorLocked is set); the active
    // block below freezes the real cursor (1px ClipCursor) and roams a raw-driven look point.
    bool lockDown = keyDown(t.cfg.cursorLockVk);
    if (lockDown && !t.lockKeyWasDown) {
        if (t.model->supportsInspect()) {
            // Snapshot cursor visibility at the toggle edge, BEFORE this tick's active block hides it,
            // together with whether WE are already hiding it. A not-showing cursor that we did not
            // hide is the mouselook-gameplay tell for game-inspect (issue #144) - true at 1x and, in
            // a transform FOLLOW session, true while zoomed as well. Both are read here so the pair
            // describes the same instant.
            CURSORINFO ci{}; ci.cbSize = sizeof(ci);
            t.inspectCursorWasShowing = GetCursorInfo(&ci) ? (ci.flags & CURSOR_SHOWING) != 0 : true;
            t.inspectMagHidCursor = t.cursorHiddenByUs;
            t.cursorLock.toggle();
        } else {
            // Magnify model: Windows Magnifier owns the view and cursor; no freeze+reticle exists.
            wind::Log(wind::LogLevel::Info, "inspect", "Inspect not available in the magnify model");
        }
    }
    t.lockKeyWasDown = lockDown;
    // Tell the mouse hook whether Inspect is on (so it swallows real clicks and routes them to the look
    // point - see the commitButton drain in the active block). Published every tick (also clears on off).
    g_input.state().inspectActive.store(t.cursorLock.locked(), std::memory_order_relaxed);
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

    int rawDx, rawDy; g_input.drainRaw(rawDx, rawDy);

    bool zoomed = lvl > 1.0;
    bool inspect = t.cursorLock.locked();
    bool active = zoomed || inspect;                 // overlay runs while zoomed OR Inspect-frozen

    if (active) {
        bool enterActive  = !t.prevActive;            // idle -> active (overlay just turned on)
        bool inspectEnter = inspect && !t.prevInspect;
        if (enterActive) {
            t.outlineIdleSec = 0.0;   // each activation starts with the outline fully shown
            // Follow the cursor's monitor (multiMonitor on, only when zoomed). Only reconfigure when
            // it actually changed; retarget() returns false on multi-GPU/failure, in which case we keep
            // the current monitor. The overlay is still at alpha 0 here, so a move never flashes.
            // RETARGET BEFORE THE ENGINE PICK: the pick must evaluate the SESSION's monitor - the
            // old order evaluated the PREVIOUS session's monitor, so switching monitors between
            // zooms could hand a secondary-monitor session the transform engine (or a primary
            // game session the render one). Retarget through the render model: it owns the
            // overlay, and hybrid may still be holding the transform half from the last session.
            if (zoomed && t.cfg.multiMonitor) {
                MonitorTarget nt = MonitorUnderCursor();
                IMagnifierModel* rt = t.mRender ? t.mRender : t.model;
                if (!SameMonitor(nt, t.mon) && rt->retarget(nt)) {
                    t.mon = nt;
                    t.mapper = CursorMapper(nt.w, nt.h, t.cfg.cursorSmoothing);
                    int nhz = DetectRefreshHz(nt.device);   // pace off the new monitor's refresh (#74)
                    if (nhz > 0) t.hz = nhz;
                }
            }
            if (t.mTransform) {
                // Hybrid engine pick, per zoom-in session (pure predicate: engine_pick.h, tested).
                // Only ever swapped here or in the settled instant-switch below, so every
                // activation's teardown calls route to the same engine that activated.
                HWND fgw = GetForegroundWindow();
                RefreshFgCache(t, fgw);
                auto* tAvail = dynamic_cast<TransformModel*>(t.mTransform);
                EnginePickInputs pin;
                pin.coversMonitor  = ForegroundCoversMonitor(t.mon);
                pin.borderless     = fgw && !(GetWindowLongPtrW(fgw, GWL_STYLE) & WS_CAPTION);
                pin.primaryMonitor = t.mon.x == 0 && t.mon.y == 0;
                pin.shellDesktop   = t.fgCacheShell;
                pin.excluded       = t.fgCacheExcluded;
                pin.churny         = t.fgCacheChurny;
                pin.tdrHarness     = t.cfg.tdrTest > 0;
                pin.desktopTransformOptIn = t.cfg.desktopTransform != 0;
                pin.inputTransformOk      = tAvail && tAvail->inputTransformAvailable();
                IMagnifierModel* pick = ShouldPickTransform(pin) ? t.mTransform : t.mRender;
                if (pick && pick != t.model) t.model = pick;
            }
            t.vbounds = QueryVirtualBounds();   // refresh cached clip-detect bounds (topology may have changed)
            POINT pt; GetCursorPos(&pt);
            t.mapper.reset(pt.x - t.mon.x, pt.y - t.mon.y);   // virtual -> local monitor coords
            t.lastSetVirtual = pt;        // baseline for the OS-cursor delta (first delta = 0)
            t.detector.reset();           // start free
            // Transform sessions run the WELDED-cursor design (re-test of the #148 weld; see
            // transform_model.cpp): the transform welds the REAL cursor to the lens point, so
            // hover, drags, and clicks are native - same contract as the render engine. The old
            // game-session FREEZE (1px clip + sprite aim point + click re-routing) was retired
            // with it; git history has the machinery if the re-test fails.
            if (dynamic_cast<TransformModel*>(t.model)) {
                // Record the session app for the device-lost churny backstop: if the GPU resets
                // within 30s of a transform game session, that app is remembered in
                // churny_apps.txt and future zoom-ins over it pick render (one crash, never two).
                t.transformExe = ExeNameOf(GetForegroundWindow());
                wind::Log(wind::LogLevel::Info, "hybrid", "transform session (welded cursor)");
            } else {
                SetSystemCursorHidden(t, t.model, true);
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
            SetSystemCursorHidden(t, t.model, true);   // hide the real cursor; we draw the crosshair
            t.model->onActivate();
            // Game-inspect (issue #144): if a mouselook game holds the mouse, the freeze alone is
            // not enough - its raw-input camera still receives every mickey. Steal foreground to
            // the invisible helper so the game stops getting input. Deferred via
            // inspectStealPending: the reveal logic later this tick must still see the GAME as
            // foreground (ForegroundCoversMonitor decides the composite-gated reveal).
            t.inspectGame = wind::ShouldGameInspect(zoomed, t.detector.locked(),
                                                    t.inspectCursorWasShowing,
                                                    t.inspectMagHidCursor);
            if (t.inspectGame) {
                t.inspectPrevFg = GetForegroundWindow();
                t.inspectStealPending = true;
            }
            // Logged either way: a DECLINE is the interesting case in the field (the camera keeps
            // moving), and without the inputs there is nothing to diagnose it from.
            wind::Log(wind::LogLevel::Info, "inspect",
                      "game-inspect %s (zoomed=%d detLocked=%d cursorShown=%d weHid=%d)",
                      t.inspectGame ? "engaged" : "DECLINED", (int)zoomed,
                      (int)t.detector.locked(), (int)t.inspectCursorWasShowing,
                      (int)t.inspectMagHidCursor);
        }
        bool inspectExit = !inspect && t.prevInspect;   // Inspect just turned off but overlay stays (zoomed)
        if (inspectExit) {
            EndGameInspect(t);   // hand foreground back to the game before resuming normal follow
            // Un-drained swallowed clicks die with the session: a click swallowed in the final
            // tick before toggle-off would otherwise linger and fire as a phantom injected click
            // at the lens centre on the NEXT activation (possibly much later, anywhere on screen).
            g_input.state().commitLeft.exchange(0);
            g_input.state().commitRight.exchange(0);
            // Leaving Inspect resumes at the LOOK POINT for BOTH models: the crosshair is what the
            // user was aiming with, so the cursor belongs there - snapping back to the pre-Inspect
            // position (which the transform model used to do, because it was forbidden from
            // placing the cursor) throws away the aim the user just spent the mode establishing.
            ClipCursor(nullptr);
            POINT lp{ (int)(t.mapper.centerX() + 0.5) + t.mon.x,
                      (int)(t.mapper.centerY() + 0.5) + t.mon.y };
            SetCursorPos(lp.x, lp.y);
            t.lastSetVirtual = lp;
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
        bool dragFollow = false;   // set in the free render branch below; drives ex.suppressCursorSync
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
            // A clip is a lock signal only when it is meaningfully SMALLER than the monitor
            // (issue #169): a machine-wide work-area clip (desktop minus taskbar, ~95%) is
            // desktop-like, not a game confining the pointer - the old any-clip test made every
            // zoomed desktop session run the locked path on this rig. See ClipRectConfines.
            bool clipConfined = wind::ClipRectConfines((int)(clip.right - clip.left),
                                                       (int)(clip.bottom - clip.top),
                                                       t.mon.w, t.mon.h);
            bool locked = t.detector.update(clipConfined,
                                            std::abs(rawDx) + std::abs(rawDy),
                                            std::abs(curDx) + std::abs(curDy));
            if (locked) {
                dx = (int)std::lround(rawDx * t.cfg.cursorSensitivity);
                dy = (int)std::lround(rawDy * t.cfg.cursorSensitivity);
            } else {
                // Drag-follow (issue #169): while a mouse button is physically held, the pointer IS
                // the interaction (window drag, text selection) - the per-tick weld would fight the
                // hand and the dragged content flickers between the two positions. Suspend the weld
                // (ex.suppressCursorSync below) and follow the pointer 1:1 unscaled: scaling would
                // desync the lens from the pointer that owns the drag. The press itself landed
                // under the welded cursor (the weld was live until the button went down), and the
                // release lands where the pointer and the dragged content both are - correct by
                // construction. Weld resumes on release. BOTH engines weld now (the transform
                // joined with the 8a52040 re-test), so both take this path.
                const bool anyButtonDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) ||
                                           (GetAsyncKeyState(VK_RBUTTON) & 0x8000) ||
                                           (GetAsyncKeyState(VK_MBUTTON) & 0x8000);
                const bool weldActive = dynamic_cast<RenderModel*>(t.model) != nullptr ||
                                        dynamic_cast<TransformModel*>(t.model) != nullptr;
                dragFollow = wind::ShouldDragFollow(weldActive, locked, inspect, anyButtonDown);
                if (dragFollow) {
                    dx = curDx;
                    dy = curDy;
                } else {
                    dx = (int)std::lround(curDx * t.cfg.cursorSensitivity);   // auto-matched OS delta, speed-scaled
                    dy = (int)std::lround(curDy * t.cfg.cursorSensitivity);
                }
            }
        }
        // Defensive: bound one tick's pan to the monitor span so a stray cursor jump (e.g. the OS
        // cursor briefly escaping to another monitor) cannot teleport the lens. cx_ also clamps.
        if (dx >  t.mon.w) dx =  t.mon.w; else if (dx < -t.mon.w) dx = -t.mon.w;
        if (dy >  t.mon.h) dy =  t.mon.h; else if (dy < -t.mon.h) dy = -t.mon.h;
        // Foreground facts for this tick, read ONCE and reused by the pan wall, the perf levers,
        // and the hybrid instant switch below (GetForegroundWindow + window queries per tick add
        // up at 144Hz, and split reads can disagree mid-tick).
        HWND fgTick = GetForegroundWindow();
        const bool fsCover = ForegroundCoversMonitor(t.mon);
        const bool fgBorderless = fgTick && !(GetWindowLongPtrW(fgTick, GWL_STYLE) & WS_CAPTION);
        // Pan wall (issue #148 final fix): transform GAME sessions on an MPO-ENABLED machine keep
        // the source rect's left edge under the driver-safe bound - the driver packs DWM's
        // magnification translation into a 16-bit overlay-plane field, so |srcX*level| > 32767
        // (the far-right strip above ~9.3x) wraps and TDRs. Keyed to the SESSION TYPE (transform
        // model + borderless cover), not any cursor state: the wall must hold in the welded
        // design too. MPO off (this rig), or render/desktop sessions: unrestricted (field-clean).
        auto* tmWall = dynamic_cast<TransformModel*>(t.model);
        const bool transformGame = tmWall != nullptr && fsCover && fgBorderless;
        const bool mpoExposed = transformGame && !g_mpoDisabled;
        // MPO buster (issue #191): a fullscreen alpha-1 ghost window demotes the game off its
        // hardware overlay plane by geometry (the parking law, PresentMon-proven) - no plane, no
        // 16-bit field, no overflow. The walls lift ONLY on verified evidence (ghost shown +
        // settled >=350ms + rect intact), never on intent: any doubt keeps them up (fail-closed).
        // tdrTest=4 is the field harness override (walls off regardless, for the repro probe).
        const bool wallNeeded = mpoExposed && t.cfg.tdrTest != 4 &&
                                !(t.cfg.mpoBuster != 0 && tmWall->mpoGhostSettled());
        t.mapper.setMaxSourceLeft(wallNeeded ? kMaxSafeTxMagnitude / lvl : -1.0);
        // Y wall too (issue #191): |srcY*level| overflows the same 16-bit field - the bottom
        // strip above ~16.2x on 2160 was reachable-lethal with the X-only wall.
        t.mapper.setMaxSourceTop(wallNeeded ? kMaxSafeTxMagnitude / lvl : -1.0);
        if (tmWall) {
            tmWall->setMpoBusterWanted(mpoExposed && t.cfg.mpoBuster != 0);
            tmWall->setMpoExposed(mpoExposed);
        }
        if (transformGame) t.lastTransformGameMs = GetTickCount64();   // device-lost backstop window
        // Launch quiesce: per-tick cover tracking while zoomed (a mid-session takeover by a
        // launching game must still arm the hold). Anchoring and once-per-process rules live in
        // TrackLaunchCover; the hold itself is consumed via QuiesceHoldActive below.
        TrackLaunchCover(t, fgTick, fsCover, fgBorderless);
        MapResult r = t.mapper.update(dx, dy, lvl);
        // Dead-zone probe (probeClicks=1, diagnostic): the field annotates hover dead zones by
        // clicking. Plain click = "hover works here" (OK), Ctrl+click = "dead here" (DEAD). Each
        // click logs every coordinate space in the chain plus what Windows hit-tests at the
        // pointer, so a divergence between the weld point, the applied DWM transform, and the
        // hit-test target names itself. Zero cost unless the knob is on AND transform is active.
        if (t.cfg.probeClicks && dynamic_cast<TransformModel*>(t.model)) {
            // Mode 2: continuous trace (every 4th tick ~36Hz) of the physical pre-weld cursor vs
            // the weld target - the physical stream is what pointer-framework apps perceive
            // (SetCursorPos emits no pointer frames; rig-proven), so a divergence here IS the
            // hover input XAML actually gets.
            if (t.cfg.probeClicks == 2 && (++t.probeTraceTick & 3) == 0) {
                auto* tw = dynamic_cast<TransformModel*>(t.model);
                wind::Log(wind::LogLevel::Info, "ptrace",
                          "lvl=%.2f pre=(%ld,%ld) weld=(%d,%d) d=(%ld,%ld) welded=%d src=(%.1f,%.1f) cs=(%.1f,%.1f)",
                          lvl, cur.x, cur.y,
                          r.clickDesktopX + t.mon.x, r.clickDesktopY + t.mon.y,
                          cur.x - (r.clickDesktopX + t.mon.x), cur.y - (r.clickDesktopY + t.mon.y),
                          tw && tw->weldedLastFrame() ? 1 : 0,
                          r.srcLeft, r.srcTop, r.cursorScreenX, r.cursorScreenY);
            }
            const bool lDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
            if (lDown && !t.probePrevLDown) {
                const bool dead = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
                POINT cp{}; GetCursorPos(&cp);
                RECT clip{}; GetClipCursor(&clip);
                float mLvl = 0.f; int mOffX = 0, mOffY = 0;
                MagGetFullscreenTransform(&mLvl, &mOffX, &mOffY);
                HWND under = WindowFromPoint(cp);
                wchar_t ucls[64]{}; if (under) GetClassNameW(under, ucls, 64);
                DWORD upid = 0; if (under) GetWindowThreadProcessId(under, &upid);
                // Cross-space check: what sits at the SCREEN position where the aim point is
                // DISPLAYED (if hover follows this instead of the weld point, spaces are mixed).
                POINT sp{ (int)(r.cursorScreenX + 0.5) + t.mon.x, (int)(r.cursorScreenY + 0.5) + t.mon.y };
                HWND underS = WindowFromPoint(sp);
                wchar_t scls[64]{}; if (underS) GetClassNameW(underS, scls, 64);
                DWORD spid = 0; if (underS) GetWindowThreadProcessId(underS, &spid);
                const DWORD ourPid = GetCurrentProcessId();
                wind::Log(wind::LogLevel::Info, "probe",
                    "%s lvl=%.2f weld=(%d,%d) cur=(%ld,%ld) src=(%.1f,%.1f) center=(%.1f,%.1f) "
                    "cursorScreen=(%.1f,%.1f) dwm(lvl=%.2f off=%d,%d) clip=(%ld,%ld)-(%ld,%ld) "
                    "underCur=%ls%s underScreen=%ls%s",
                    dead ? "DEAD" : "OK", lvl,
                    r.clickDesktopX + t.mon.x, r.clickDesktopY + t.mon.y, cp.x, cp.y,
                    r.srcLeft, r.srcTop, r.centerX, r.centerY,
                    r.cursorScreenX, r.cursorScreenY, mLvl, mOffX, mOffY,
                    clip.left, clip.top, clip.right, clip.bottom,
                    ucls, upid == ourPid ? L" (OURS)" : L"",
                    scls, spid == ourPid ? L" (OURS)" : L"");
            }
            t.probePrevLDown = lDown;
        }
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
                if (dynamic_cast<TransformModel*>(t.model)) {
                    // The injected absolute move races transform writes (issue #148 TDR class), so
                    // SERIALIZE: skip transform writes for a couple ticks around it (ex.pauseWrites).
                    t.clickPauseTicks = 3;
                }
            }
        }
        // Fullscreen-game tell for the per-tick perf levers (issue #148): crop the capture copy to
        // the magnified region (gameCrop), skip the periodic topmost backstop, and optionally cap
        // our own present rate (gameFpsCap). fgTick/fsCover/fgBorderless were read once above.
        const bool fsGame = fsCover;
        // Instant hybrid switch (always on): re-pick the engine WHILE ZOOMED when the foreground
        // changes, handing over mid-session. The controller and mapper are untouched, so the
        // zoom level and lens position carry across the swap (render 8x -> tab into a game ->
        // transform 8x). Inspect sessions are never switched under.
        // Freeze the engine choice while an overlay is in front (see IsOverlayFg). Skipping the
        // whole block, rather than just the swap, is deliberate: it leaves wantModel/wantSinceMs
        // untouched, so the overlay never becomes the settled candidate and handing foreground back
        // is a no-op instead of a second handover.
        if (t.mTransform && !enterActive && !inspect && !IsOverlayFg(fgTick)) {
            // Same pure predicate as the zoom-in pick (engine_pick.h) - the two sites can no
            // longer drift apart. Exe-derived inputs come from the per-HWND cache: this block
            // runs every zoomed tick and the process-open per tick was measurable waste.
            RefreshFgCache(t, fgTick);
            auto* tAvail = dynamic_cast<TransformModel*>(t.mTransform);
            EnginePickInputs pin;
            pin.coversMonitor  = fsCover;
            pin.borderless     = fgBorderless;
            pin.primaryMonitor = t.mon.x == 0 && t.mon.y == 0;
            pin.shellDesktop   = t.fgCacheShell;
            pin.excluded       = t.fgCacheExcluded;
            pin.churny         = t.fgCacheChurny;
            pin.tdrHarness     = t.cfg.tdrTest > 0;
            pin.desktopTransformOptIn = t.cfg.desktopTransform != 0;
            pin.inputTransformOk      = tAvail && tAvail->inputTransformAvailable();
            IMagnifierModel* want = ShouldPickTransform(pin) ? t.mTransform : t.mRender;
            // STICKY (field: the engine flapped render<->transform inside one zoom session, and
            // each flip releases and rebuilds DWM's magnification context - a stall every time).
            // A real alt-tab still switches; a one-frame wobble in the foreground reads does not.
            if (want != t.wantModel) { t.wantModel = want; t.wantSinceMs = GetTickCount64(); }
            const bool wantSettled = GetTickCount64() - t.wantSinceMs >= 350;
            const bool fgIsStealer = fgTick && fgTick == g_focusStealer;   // game-inspect helper holds fg
            if (want && want != t.model && wantSettled && !fgIsStealer) {
                if (t.restAfterReveal) {   // rapid double-switch: settle the previous handover
                    t.restAfterReveal->setActive(false);
                    t.restAfterReveal = nullptr;
                    t.restOverlapTicks = 0;
                }
                IMagnifierModel* old = t.model;
                SetSystemCursorHidden(t, old, false);
                t.model = want;
                // Both engines weld the cursor to the lens point, so the handover is seamless:
                // render hides the OS cursor immediately; the transform hides it in its own
                // present (level > 1.001) and re-welds at the same point.
                if (dynamic_cast<RenderModel*>(t.model)) SetSystemCursorHidden(t, t.model, true);
                else t.transformExe = ExeNameOf(fgTick);   // device-lost backstop attribution
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
                // Name the trigger: if another transient surface ever ping-pongs the engine the
                // way the taskbar flyout did (issue #180), the log identifies it directly.
                wchar_t fgCls[64]{};
                if (fgTick) GetClassNameW(fgTick, fgCls, 64);
                wind::Log(wind::LogLevel::Info, "hybrid",
                          "instant switch -> %s (level preserved; fg cls=%ls exe=%ls)",
                          dynamic_cast<RenderModel*>(t.model) ? "render" : "transform",
                          fgCls, ExeNameOf(fgTick).c_str());
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
        // Drag-follow (issue #169): the pan resolve above chose to follow the pointer this tick, so
        // neither engine may weld it back to the lens centre - suspend the SetCursorPos.
        ex.suppressCursorSync = dragFollow;
        ex.moveSignals = g_input.state().moveSignals.exchange(0, std::memory_order_relaxed);
        // Serialize transform writes around an Inspect click's injected absolute move (issue #148
        // TDR class): the injection and a transform write racing each other is the proven trigger.
        // The launch quiesce holds writes AND the weld until its deadline (anchored at cover
        // sighting, not zoom-in - see TrackLaunchCover; deadline-based, so a short press can
        // never strand leftover hold into the next session).
        const bool quiesceHold = QuiesceHoldActive(t);
        ex.pauseWrites = t.clickPauseTicks > 0 || quiesceHold;
        if (quiesceHold) ex.suppressCursorSync = true;
        if (t.clickPauseTicks > 0) --t.clickPauseTicks;
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
        // Bookkeeping for next tick's GetCursorPos delta. INSPECT keeps the explicit frozen point
        // (the 1px clip pins the pointer there; explicit is immune to the click-release window).
        // Everything else uses a MEASURED read (issue #169) - the baseline must be where the
        // pointer actually IS after this tick's present, never where we intended to put it:
        //  - welded (render park OR transform weld; both report whether SetCursorPos REALLY ran
        //    this frame): the call is synchronous, so the read equals the park/weld point -
        //    identical to the old assumed baseline.
        //  - weld deduped/suppressed (unchanged centre pixel, drag-follow, gatePresent or
        //    fps-cap skip ticks): the old code assumed the park landed anyway and baselined on
        //    the lens centre. The next delta then measured hand + (pointer - centre) gap, the
        //    mapper integrated the gap, the centre overshot the pointer, and the sign flipped
        //    every tick: an unstable servo, oscillating with amplitude proportional to hand
        //    speed. That was the #169 window-drag flicker - and, before the transform weld was
        //    recognized here, the #181 corner drift in transform game sessions.
        if (inspect) {
            t.lastSetVirtual = t.frozenCursor;
        } else {
            // Parked -> the park point: hand motion after the park (including during the Present
            // block) is measured next tick from there. Not parked (weld deduped/suppressed,
            // transform FOLLOW, skipped frame) -> THIS tick's start-of-tick read `cur`: motion
            // after that read is measured next tick. NEVER a fresh post-present read - Present
            // blocks ~a frame at vsync, and a read taken after it swallows all hand motion that
            // occurred during the block, so the lens pans slower than the hand and the weld drags
            // the pointer backwards (field-reported as stalling/slowed cursor; first shipped
            // version of this fix had exactly that bug).
            auto* rmodel = dynamic_cast<RenderModel*>(t.model);
            auto* tweld  = dynamic_cast<TransformModel*>(t.model);
            const bool parkedNow = doPresent &&
                ((rmodel && rmodel->engine().parkedLastFrame()) ||
                 (tweld && tweld->weldedLastFrame()));
            if (parkedNow) {
                t.lastSetVirtual.x = r.clickDesktopX + t.mon.x;
                t.lastSetVirtual.y = r.clickDesktopY + t.mon.y;
            } else {
                t.lastSetVirtual = cur;
            }
            // Divergence diagnostics (issue #169, diagnostics=1 only): once a second, log how far
            // the pointer sits from the lens centre and how many ticks drag-followed. If any
            // oscillation survives the fix, this pinpoints the fighting pair from the field log.
            if (t.cfg.diagnostics) {
                const double divX = t.lastSetVirtual.x - (double)(r.clickDesktopX + t.mon.x);
                const double divY = t.lastSetVirtual.y - (double)(r.clickDesktopY + t.mon.y);
                const double div = std::sqrt(divX * divX + divY * divY);
                if (div > t.dbgMaxDivergence) t.dbgMaxDivergence = div;
                if (dragFollow) t.dbgDragFollowTicks++;
                const unsigned long long nowD = GetTickCount64();
                if (nowD - t.dbgDivergenceLogMs >= 1000) {
                    if (t.dbgDivergenceLogMs != 0) {
                        wind::Log(wind::LogLevel::Info, "cursor",
                                  "divergence max=%.0fpx dragFollowTicks=%u lvl=%.2f",
                                  t.dbgMaxDivergence, t.dbgDragFollowTicks, lvl);
                    }
                    t.dbgDivergenceLogMs = nowD;
                    t.dbgMaxDivergence = 0.0;
                    t.dbgDragFollowTicks = 0;
                }
            }
        }
    } else if (t.prevActive) {                        // active -> idle: tear the overlay down
        if (t.restAfterReveal) { t.restAfterReveal->setActive(false); t.restAfterReveal = nullptr; }
        t.model->setActive(false);
        SetSystemCursorHidden(t, t.model, false);
        t.outlineZoneSec = 0.0;                       // zoom-out clears the low-zoom dwell (no banked partial)
        t.gamePacing = false;                         // idle: normal timer pacing
        t.pushPhase = 0;
        t.presentAccum = 0.0;
        if (t.prevInspect) {
            EndGameInspect(t);   // teardown-to-idle exits game-inspect too (foreground returned)
            ClipCursor(nullptr);
            // Same phantom-click guard as the zoomed Inspect exit: swallowed-but-undrained clicks
            // must not survive into the next activation.
            g_input.state().commitLeft.exchange(0);
            g_input.state().commitRight.exchange(0);
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
            // Re-fire every 5 s while a hold overstays, not once. A stuck hold has no falling edge,
            // so a single sample can never show HOW FAST the runaway zoom is climbing - and that
            // rate (reported slower than the configured zoomInSpeed) is the open half of #167. A
            // series of lvl= samples 5 s apart makes it computable from the log. Gated on the
            // 5 s window rather than a flag, so it costs no extra state and cannot spam per tick.
            } else if (held && secs > 6.0 &&
                       static_cast<int>(secs / 5.0) != static_cast<int>((secs - dt) / 5.0)) {
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
        // Watch the EFFECTIVE held state - side-buttons OR keyboard binds - not just the
        // side-button half (issue #167). A stuck KEYBOARD bind drove a runaway zoom that this
        // detector could not see, which is why episodes of #167 never left a STUCK? line in the
        // log despite being hit repeatedly. inHeld/outHeld are the same values that drive the
        // zoom, so the diagnostic now reports what actually happened rather than half of it.
        snap("in",  inHeld,  t.dbgPrevInHeld,  t.dbgInHeldSec,  t.dbgInOverstayLogged);
        snap("out", outHeld, t.dbgPrevOutHeld, t.dbgOutHeldSec, t.dbgOutOverstayLogged);
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
        // One syscall, not two: RAWINPUT for mouse/keyboard always fits the fixed buffer, so the
        // size-query round trip per packet (hundreds/s while panning) bought nothing.
        alignas(8) BYTE buf[128];
        UINT size = sizeof(buf);
        UINT got = GetRawInputData((HRAWINPUT)lp, RID_INPUT, buf, &size, sizeof(RAWINPUTHEADER));
        if (got != (UINT)-1 && got > 0) {
            auto* ri = reinterpret_cast<RAWINPUT*>(buf);
            if (ri->header.dwType == RIM_TYPEKEYBOARD) {
                // Keyboard UP only, and deliberately the exact mirror of the side-button rule
                // below (issue #167). A key UP can only ever CLEAR held state, never set it, so
                // honoring it unconditionally is a pure safety net: idempotent with the hook's own
                // clear, and incapable of falsely holding a key. DOWN stays hook-authoritative -
                // the hook owns the swallow decision and the rising edge, and setting DOWN here
                // would double-count and could disagree with it for a tick.
                //
                // Without this, a hook evicted mid-hold (a heavy process's first load spike is
                // long enough to blow LowLevelHooksTimeout) never sees the release: the zoom then
                // runs forever and the stale "held" bit also hides the dead hook from the watchdog
                // above, since a live hook swallows bound keys so the poller can never see one.
                const RAWKEYBOARD& kb = ri->data.keyboard;
                if ((kb.Flags & RI_KEY_BREAK) && kb.VKey > 0 && kb.VKey < 256)
                    g_input.rawKeyUp(static_cast<int>(kb.VKey));
            } else if (ri->header.dwType == RIM_TYPEMOUSE) {
                const RAWMOUSE& m = ri->data.mouse;
                if ((m.usFlags & MOUSE_MOVE_ABSOLUTE) == 0) {
                    AccumulateRaw(g_input, m.lLastX, m.lLastY);
                }
                // Side-button held state. The button DOWN edge stays HOOK-authoritative when the
                // LL hook is active (it owns the swallow/edge logic; writing DOWN here too would
                // double-count and could momentarily disagree with the hook's view) - so DOWN is
                // decoded here only as the WIND_NOHOOK fallback. The button UP is
                // honored from Raw Input: an LL hook can be silently skipped by Windows on a
                // LowLevelHooksTimeout stall, and a dropped XBUTTON UP would otherwise strand the
                // button as held (intermittent stuck-zoom, recovers only on a re-click). Raw Input
                // is delivered through a path NOT subject to that timeout, and a UP can only CLEAR
                // held-state, never set it, so processing it unconditionally is a pure safety net
                // (idempotent with the hook's own clear; never falsely holds). It does not touch the
                // hook's g_swallowedDown record, so swallowing is unaffected.
                USHORT bf = m.usButtonFlags;
                if (bf & RI_MOUSE_BUTTON_4_UP) g_input.rawButtonUp(1);
                if (bf & RI_MOUSE_BUTTON_5_UP) g_input.rawButtonUp(2);
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

    // Profiles (spec 2026-08-12): first launch after the update seeds profiles\Default.ini from the
    // user's current settings, so existing installs get a "Default" profile with zero user action.
    EnsureProfilesSeeded(iniPath);

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

    // Mouse AND keyboard (issue #167). The keyboard registration exists for exactly one reason: an
    // LL hook can be silently evicted mid-hold, and the key UP then reaches nobody, stranding a
    // keyboard zoom bind as held forever. Raw Input is not subject to LowLevelHooksTimeout, so it
    // still delivers that UP. This is the same safety net the side-buttons have had since #113;
    // keyboard binds never got it. RIDEV_INPUTSINK so it arrives regardless of foreground.
    RAWINPUTDEVICE rid[2]{};
    rid[0].usUsagePage = 0x01; rid[0].usUsage = 0x02; // generic mouse
    rid[0].dwFlags = RIDEV_INPUTSINK; rid[0].hwndTarget = hwnd;
    rid[1].usUsagePage = 0x01; rid[1].usUsage = 0x06; // generic keyboard
    rid[1].dwFlags = RIDEV_INPUTSINK; rid[1].hwndTarget = hwnd;
    RegisterRawInputDevices(rid, 2, sizeof(rid[0]));

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
                    cfg.cursorLockVk);

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
        auto tm = std::make_unique<TransformModel>(cfg.fastPan != 0, cfg.smoothPan != 0,
                                                   cfg.cursorSprite != 0, cfg.zorderBand,
                                                   cfg.spriteBand16 != 0);
        tm->setIdleReleaseMs(cfg.txIdleReleaseMs);
        model = std::move(tm);
    } else {
        model = std::make_unique<RenderModel>(cfg.zorderBand, cfg.hdrTonemap != 0,
                                              EffectiveGpuPriority(cfg));
        if (cfg.model == "hybrid") {
            // Hybrid (issue #148): render model on the desktop (centered cursor, unlimited
            // levels), transform model whenever a fullscreen app is foreground at zoom-in
            // (compositor-internal - the only path that stays smooth over a heavy game). The
            // engine is picked per zoom-in session in RunTick; both stay initialized.
            auto tm2 = std::make_unique<TransformModel>(cfg.fastPan != 0, cfg.smoothPan != 0,
                                                        cfg.cursorSprite != 0, cfg.zorderBand,
                                                        cfg.spriteBand16 != 0);
            tm2->setIdleReleaseMs(cfg.txIdleReleaseMs);
            model2 = std::move(tm2);
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
        }
        // Unconditional MODEL shutdown (not just the render engine): a non-render model still
        // holds cursor/runtime state that must be restored on this early exit path.
        model->shutdown();
        if (model2) model2->shutdown();
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
        }
        // Unconditional MODEL shutdown, same as the selftest exit above.
        model->shutdown();
        if (model2) model2->shutdown();
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
            if (!ts.transformExe.empty() &&
                GetTickCount64() - ts.lastTransformGameMs < 30000) {
                MarkChurnyApp(ts.transformExe, "device-lost backstop");
            }
            // Restore through the ACTIVE model: in a transform session the transform half (not
            // the render engine) hid the cursor, and only it restores its blanker state too.
            SetSystemCursorHidden(ts, ts.model, false);
            // Inspect's 1px freeze clip must not survive a device-lost: release it and clear the toggle so
            // the post-recovery tick can't re-clip the cursor to the stale frozen pixel (honors the
            // documented "released on device-lost recovery" invariant; recovery returns to a clean 1x).
            ClipCursor(nullptr);
            if (ts.restAfterReveal) { ts.restAfterReveal->setActive(false); ts.restAfterReveal = nullptr; }
            if (ts.cursorLock.locked()) {
                ts.cursorLock.reset(); ts.clickReleaseTicks = 0;
                // Un-drained swallowed clicks must die with the Inspect session, or the next
                // zoom-in drains them and fires a phantom injected click at the lens centre.
                g_input.state().commitLeft.exchange(0);
                g_input.state().commitRight.exchange(0);
            }
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

        // HIGH-RATE CURSOR REPAN (issue #195, the wobble fix). DWM draws the magnified cursor
        // from its LIVE position at composite time, but uses the offset we last WROTE. At one
        // write per composite that offset is up to a frame stale, so the view trails the cursor
        // by hand-speed * staleness * level: the visible lead while moving, and - because the
        // write-to-composite gap jitters frame to frame - the residual wobble, both growing
        // with zoom exactly as reported. Native has the same algebra but writes from a
        // low-level mouse hook, i.e. sub-millisecond staleness (agent disassembly: threadpool
        // timer + WH_MOUSE_LL, no compositor sync). Match that by polling the cursor through
        // the pre-composite wait and re-panning on change, so what DWM samples is always fresh.
        // Cost while the hand is still: one GetCursorPos per poll and no write at all.
        // The Magnification API is thread-affine, so this must stay on the tick thread (an
        // async writer was measured-negative) - hence polling here rather than in the hook.
        if (dwmPaces) DwmFlush();   // block until DWM's next composite -> frames align with it

        // FIXED-PHASE REPAN (issue #195, measured). DwmFlush returns immediately after a
        // composite, so writing HERE pins the pan write to a stable point in the frame - and a
        // stable write-to-latch distance is what makes the cursor sit still. Measured why this
        // shape: the wobble was always ~one frame of TIME jitter (5.8-7.6ms) no matter the write
        // rate (30/s, 300/s, 1000/s all identical), i.e. it is not staleness but PHASE variance -
        // our single per-frame write landed wherever the tick happened to end. The old
        // event-driven poll could not fix that: instrumentation showed the hook delivering ~1
        // move per tick, so the loop simply waited out its budget and wrote once anyway.
        // The constant lead this leaves (one frame) is cancelled by txCursorLeadMs prediction,
        // which is exactly the quantity a fixed phase makes predictable.
        if (dwmPaces && ts.prevLvl > 1.001 && ts.cfg.txCursorPollHz > 0) {
            if (auto* tmFast = dynamic_cast<TransformModel*>(ts.model)) {
                // Repan through the frame on a SPIN cadence, not an event and not a waitable
                // timer: the mouse-move event proved to deliver ~1 wake per tick (instrumented),
                // and a 1ms waitable timer only achieved ~3ms in practice. Composite-synced
                // measurement showed the displayed offset alternating between fresh and ~15ms
                // stale, so what matters is that a RECENT value is always in place whenever DWM
                // latches - i.e. small worst-case staleness, not average write rate. Spinning is
                // affordable here (zoomed transform sessions only) and gives ~1ms worst case.
                const int hzNow = ts.hz > 0 ? ts.hz : 60;
                const long long budgetUs = (1000000LL / hzNow) * 3 / 4;
                const long long stepUs = 1000000LL / (ts.cfg.txCursorPollHz > 0 ? ts.cfg.txCursorPollHz : 1000);
                LARGE_INTEGER pf, pa, pb;
                QueryPerformanceFrequency(&pf);
                QueryPerformanceCounter(&pa);
                long long nextUs = 0;
                int idleSpins = 0;
                for (;;) {
                    QueryPerformanceCounter(&pb);
                    const long long spent = (pb.QuadPart - pa.QuadPart) * 1000000LL / pf.QuadPart;
                    if (spent >= budgetUs) break;
                    if (spent >= nextUs) {
                        nextUs = spent + stepUs;
                        // A still hand needs no refresh: bail out of the spin once several
                        // consecutive repans find nothing to write, so an idle zoomed session
                        // costs nothing. Any motion resumes it on the next frame.
                        // fastCursorRepan reports POINTER ACTIVITY (moved or wrote), so the
                        // bail-out only triggers on a genuinely still hand - ~20ms of no motion.
                        if (tmFast->fastCursorRepan(ts.cfg)) idleSpins = 0;
                        else if (++idleSpins >= 20) break;
                    } else {
                        YieldProcessor();
                    }
                }
            }
        }
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
    // Hybrid holds TWO models; quitting while zoomed in (or shortly after) a transform session
    // left the transform half's magnification context + cursor state untouched without this.
    if (model2) model2->shutdown();
    g_input.stop();
    Tray::Remove();
    if (mtx) { ReleaseMutex(mtx); CloseHandle(mtx); }
    wind::LogShutdown();
    return 0;
}
