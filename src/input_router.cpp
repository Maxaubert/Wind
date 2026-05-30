#include "input_router.h"
#include <windows.h>
#include <atomic>
namespace wind {
static InputRouter* g_router = nullptr;
static HHOOK   g_mouseHook    = nullptr;
// The WH_MOUSE_LL hook lives on its OWN thread (see start()): Windows services a low-level hook on
// the thread that installed it and holds each mouse event until that thread responds, so the hook
// MUST sit on a thread that pumps messages constantly. On the main thread it was starved behind the
// per-frame render/pacing block, batching all system mouse input by a frame (in-game microstutter).
static HANDLE  g_hookThread   = nullptr;
static DWORD   g_hookThreadId = 0;
static HANDLE  g_hookReady    = nullptr;   // signaled by the thread once the hook is installed (or failed)
static bool    g_hookOk       = false;     // result of SetWindowsHookExW, published via g_hookReady
// Per-side-button record of whether we swallowed the DOWN (index by id: 1=XBUTTON1, 2=XBUTTON2).
// Only an UP whose DOWN we swallowed may be swallowed too, so the system's down/up view stays
// balanced and a button can never be left believed-held. Reset on remap so a stale flag from a
// previous binding can't cause a later UP to be wrongly swallowed. ATOMIC: touched by three
// contexts - the hook thread (MouseProc), the tick thread (setButtons on hot-reload), and the
// teardown caller (ReleaseSwallowedButtons via stop()) - so plain bools would be a data race.
static std::atomic<bool> g_swallowedDown[3] = {};

static int xbuttonIdFromHook(WPARAM wParam, LPARAM lParam) {
    auto* mi = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);
    if (wParam == WM_XBUTTONDOWN || wParam == WM_XBUTTONUP) {
        WORD hi = HIWORD(mi->mouseData); // XBUTTON1 or XBUTTON2
        return (hi == XBUTTON1) ? 1 : (hi == XBUTTON2 ? 2 : 0);
    }
    return 0;
}

// Shared by the WH_MOUSE_LL hook (below) and main's WM_INPUT path: map an XBUTTON id to held.
void InputRouter::setButtonState(int xbuttonId, bool down) {
    if (xbuttonId == inButtonId_.load(std::memory_order_relaxed))  state_.inHeld.store(down);
    if (xbuttonId == outButtonId_.load(std::memory_order_relaxed)) state_.outHeld.store(down);
}
bool InputRouter::isZoomButton(int xbuttonId) const {
    return xbuttonId == inButtonId_.load(std::memory_order_relaxed)
        || xbuttonId == outButtonId_.load(std::memory_order_relaxed);
}
void InputRouter::setButtons(int inButtonId, int outButtonId) {
    inButtonId_.store(inButtonId, std::memory_order_relaxed);
    outButtonId_.store(outButtonId, std::memory_order_relaxed);
    // Clear any held state from the previous mapping (else a press of the OLD button that was in
    // progress would never get its UP event matched and inHeld/outHeld would stick true).
    state_.inHeld.store(false);
    state_.outHeld.store(false);
    // Also clear the swallowed-DOWN records: a remap mid-press (exactly what keybind capture does)
    // must not let a stale flag cause a later, unrelated UP to be swallowed (-> stuck button).
    g_swallowedDown[1].store(false); g_swallowedDown[2].store(false);
}

static LRESULT CALLBACK MouseProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION && g_router) {
        int id = xbuttonIdFromHook(wParam, lParam);
        bool down = (wParam == WM_XBUTTONDOWN);
        bool up   = (wParam == WM_XBUTTONUP);
        if (id != 0 && (down || up)) {
            g_router->setButtonState(id, down);
            bool swallow = false;
            if (down) {
                // Swallow the DOWN only if it is a zoom button now; remember it so the matching UP
                // is swallowed too (keeps the system's down/up view balanced).
                if (g_router->swallowEnabled() && g_router->isZoomButton(id)) {
                    g_swallowedDown[id].store(true);
                    swallow = true;
                }
            } else { // up: swallow iff we swallowed its DOWN. Never swallow an UP whose DOWN the
                     // system already saw - that is exactly what left the button stuck-down.
                if (g_swallowedDown[id].exchange(false)) {
                    swallow = true;
                }
            }
            if (swallow) return 1; // swallow so browser back/forward don't fire
        }
    }
    return CallNextHookEx(g_mouseHook, code, wParam, lParam);
}

// Dedicated hook thread: installs the LL mouse hook and does nothing but pump messages so the hook
// is serviced with microsecond latency. A low-level hook callback is delivered while the owning
// thread is in a message-retrieval call (GetMessage), so this loop services every event instantly.
static DWORD WINAPI HookThreadProc(LPVOID) {
    MSG msg;
    PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);   // force a message queue to exist
    g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, MouseProc, GetModuleHandleW(nullptr), 0);
    g_hookOk = (g_mouseHook != nullptr);
    SetEvent(g_hookReady);                                        // publish the install result to start()
    if (!g_mouseHook) return 1;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {                // WM_QUIT (posted by stop()) ends this
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    UnhookWindowsHookEx(g_mouseHook);                            // unhook on the installing thread
    g_mouseHook = nullptr;
    return 0;
}

bool InputRouter::start(int inButtonId, int outButtonId, bool swallow) {
    g_router = this;
    inButtonId_.store(inButtonId, std::memory_order_relaxed);
    outButtonId_.store(outButtonId, std::memory_order_relaxed);
    swallow_ = swallow;
    // Diagnostic: WIND_NOHOOK=1 skips the low-level mouse hook entirely (button state still arrives
    // via Raw Input). Kept as a fallback / A-B toggle; side-button swallowing is disabled in it.
    if (GetEnvironmentVariableW(L"WIND_NOHOOK", nullptr, 0) > 0) {
        g_mouseHook = nullptr;
        return true;
    }
    // Install the hook on its own thread (see HookThreadProc / g_hookThread comment). The hook must
    // be installed by the thread that services it, so SetWindowsHookExW runs inside the thread proc;
    // we block here only until it reports success/failure via g_hookReady.
    g_hookReady = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_hookReady) return false;
    g_hookThread = CreateThread(nullptr, 0, HookThreadProc, nullptr, 0, &g_hookThreadId);
    if (!g_hookThread) { CloseHandle(g_hookReady); g_hookReady = nullptr; return false; }
    WaitForSingleObject(g_hookReady, INFINITE);
    CloseHandle(g_hookReady); g_hookReady = nullptr;
    hookActive_.store(g_hookOk);   // hook is now the sole button-state authority (see hookActive())
    return g_hookOk;
    // Raw Input registration (RIDEV_INPUTSINK) + WM_INPUT decoding live in main.cpp's
    // message-only window, which calls AccumulateRaw() with the decoded deltas.
}
// Synthesize an XBUTTON UP for any side-button whose DOWN we swallowed but whose UP we have not yet
// seen/passed through. Called when the hook is torn down: if we vanish mid-press (e.g. another
// instance signals us to quit while a side-button DOWN is outstanding, or shutdown races a press),
// the system would otherwise be left believing the button is held forever, breaking clicks
// system-wide. This GUARANTEES we never strand a button no matter how teardown is triggered.
static void ReleaseSwallowedButtons() {
    for (int id = 1; id <= 2; ++id) {
        if (!g_swallowedDown[id].exchange(false)) continue;
        INPUT in{};
        in.type = INPUT_MOUSE;
        in.mi.dwFlags = MOUSEEVENTF_XUP;
        in.mi.mouseData = (id == 1) ? XBUTTON1 : XBUTTON2;
        SendInput(1, &in, sizeof(in));
    }
}
void InputRouter::stop() {
    if (g_hookThread) {
        PostThreadMessageW(g_hookThreadId, WM_QUIT, 0, 0);   // break the thread's GetMessage loop
        WaitForSingleObject(g_hookThread, INFINITE);          // it unhooks itself on the way out
        CloseHandle(g_hookThread); g_hookThread = nullptr; g_hookThreadId = 0;
    } else if (g_mouseHook) {                                 // hookless/no-thread paths: unhook directly
        UnhookWindowsHookEx(g_mouseHook); g_mouseHook = nullptr;
    }
    ReleaseSwallowedButtons();   // never leave a swallowed side-button stranded as held
    hookActive_.store(false);
    g_router = nullptr;
}
void InputRouter::drainRaw(int& dx, int& dy) {
    dx = state_.rawDx.exchange(0);
    dy = state_.rawDy.exchange(0);
}

void AccumulateRaw(InputRouter& r, int dx, int dy) {
    r.state().rawDx.fetch_add(dx);
    r.state().rawDy.fetch_add(dy);
}
}
