#include "input_router.h"
#include "config.h"     // IsForbiddenBindVk (keyboard-bind safety blocklist)
#include <windows.h>
#include <atomic>
namespace wind {
static InputRouter* g_router = nullptr;
static HHOOK   g_mouseHook    = nullptr;
static HHOOK   g_kbHook       = nullptr;   // WH_KEYBOARD_LL, shares g_hookThread with the mouse hook
static bool    g_kbOk         = false;     // result of the keyboard SetWindowsHookExW, via g_hookReady
// Per-VK keyboard state (index = Virtual-Key code, 0..255). Touched by the hook thread (KbProc), the
// tick thread (setKeys / main's keyPressed reads), and teardown (ReleaseSwallowedKeys via stop()),
// so they must be atomic. g_kbPressed = physical down-state (the authority while the hook is active,
// since a swallowed key never appears in GetAsyncKeyState). g_kbSwallowedDown = whether we swallowed
// the DOWN, so only the matching UP is swallowed too (keeps the system's down/up view balanced and a
// key can never be left believed-held).
static std::atomic<bool> g_kbPressed[256]      = {};
static std::atomic<bool> g_kbSwallowedDown[256] = {};
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
// Inspect-mode click routing: a real left/right press while Inspect is on is swallowed (it would land
// at the frozen cursor, not where the crosshair is aiming); the tick fires a clean click at the look
// point instead. g_commitBtn remembers which button so the matching real UP is swallowed too. The
// tick's injected click carries LLMHF_INJECTED, so it is not re-swallowed. Hook-thread-local.
static int g_commitBtn = 0;   // 0=none, 1=left, 2=right

static int xbuttonIdFromHook(WPARAM wParam, LPARAM lParam) {
    auto* mi = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);
    if (wParam == WM_XBUTTONDOWN || wParam == WM_XBUTTONUP) {
        WORD hi = HIWORD(mi->mouseData); // XBUTTON1 or XBUTTON2
        return (hi == XBUTTON1) ? 1 : (hi == XBUTTON2 ? 2 : 0);
    }
    return 0;
}

// Shared by the WH_MOUSE_LL hook (below) and main's WM_INPUT path: map an XBUTTON id to held.
// A direction holds if the pressed button matches EITHER its primary or alternate binding.
void InputRouter::setButtonState(int xbuttonId, bool down) {
    if (xbuttonId == inButtonId_.load(std::memory_order_relaxed)
     || xbuttonId == inButtonId2_.load(std::memory_order_relaxed))  state_.inHeld.store(down);
    if (xbuttonId == outButtonId_.load(std::memory_order_relaxed)
     || xbuttonId == outButtonId2_.load(std::memory_order_relaxed)) state_.outHeld.store(down);
}
bool InputRouter::isZoomButton(int xbuttonId) const {
    return xbuttonId == inButtonId_.load(std::memory_order_relaxed)
        || xbuttonId == inButtonId2_.load(std::memory_order_relaxed)
        || xbuttonId == outButtonId_.load(std::memory_order_relaxed)
        || xbuttonId == outButtonId2_.load(std::memory_order_relaxed);
}
void InputRouter::setButtons(int inButtonId, int inButtonId2, int outButtonId, int outButtonId2) {
    inButtonId_.store(inButtonId, std::memory_order_relaxed);
    inButtonId2_.store(inButtonId2, std::memory_order_relaxed);
    outButtonId_.store(outButtonId, std::memory_order_relaxed);
    outButtonId2_.store(outButtonId2, std::memory_order_relaxed);
    // Clear any held state from the previous mapping (else a press of the OLD button that was in
    // progress would never get its UP event matched and inHeld/outHeld would stick true).
    state_.inHeld.store(false);
    state_.outHeld.store(false);
    // Also clear the swallowed-DOWN records: a remap mid-press (exactly what keybind capture does)
    // must not let a stale flag cause a later, unrelated UP to be swallowed (-> stuck button).
    g_swallowedDown[1].store(false); g_swallowedDown[2].store(false);
}

bool InputRouter::isBoundKey(int vk) const {
    if (vk <= 0 || vk > 255 || IsForbiddenBindVk(vk)) return false;   // never track/swallow forbidden keys
    return vk == kbZoomInVk_.load(std::memory_order_relaxed)
        || vk == kbZoomInVk2_.load(std::memory_order_relaxed)
        || vk == kbZoomOutVk_.load(std::memory_order_relaxed)
        || vk == kbZoomOutVk2_.load(std::memory_order_relaxed)
        || vk == kbRecenterVk_.load(std::memory_order_relaxed)
        || vk == kbCursorLockVk_.load(std::memory_order_relaxed);
}
bool InputRouter::keyPressed(int vk) const {
    if (vk <= 0 || vk > 255) return false;
    return g_kbPressed[vk].load(std::memory_order_relaxed);
}
void InputRouter::setKeys(int zoomInVk, int zoomInVk2, int zoomOutVk, int zoomOutVk2, int recenterVk,
                          int cursorLockVk) {
    kbZoomInVk_.store(zoomInVk,    std::memory_order_relaxed);
    kbZoomInVk2_.store(zoomInVk2,  std::memory_order_relaxed);
    kbZoomOutVk_.store(zoomOutVk,  std::memory_order_relaxed);
    kbZoomOutVk2_.store(zoomOutVk2,std::memory_order_relaxed);
    kbRecenterVk_.store(recenterVk,std::memory_order_relaxed);
    kbCursorLockVk_.store(cursorLockVk, std::memory_order_relaxed);
    // Clear per-key pressed + swallowed records so a remap mid-press (keybind capture clears the old
    // binding) can't leave a held flag stuck or cause a later, unrelated UP to be swallowed.
    for (int i = 0; i < 256; ++i) { g_kbPressed[i].store(false); g_kbSwallowedDown[i].store(false); }
}

static LRESULT CALLBACK KbProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION && g_router) {
        auto* ks = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
        int vk = static_cast<int>(ks->vkCode);
        bool down = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
        bool up   = (wParam == WM_KEYUP   || wParam == WM_SYSKEYUP);
        // Only bound (non-forbidden) keys are tracked/swallowed; every other keystroke passes through
        // untouched. isBoundKey already range-checks vk and excludes IsForbiddenBindVk keys.
        if ((down || up) && g_router->isBoundKey(vk)) {
            bool swallow = false;
            if (down) {
                // Auto-repeat re-fires WM_KEYDOWN; storing true each time is idempotent. main reads
                // this as the physical down-state and does its own rising-edge work for taps.
                g_kbPressed[vk].store(true);
                if (g_router->swallowEnabled()) {
                    g_kbSwallowedDown[vk].store(true);
                    swallow = true;
                }
            } else { // up: swallow iff we swallowed its DOWN, so the system's down/up view stays balanced.
                g_kbPressed[vk].store(false);
                if (g_kbSwallowedDown[vk].exchange(false)) swallow = true;
            }
            if (swallow) return 1; // eat the key so the focused app never sees the zoom/recenter bind
        }
    }
    return CallNextHookEx(g_kbHook, code, wParam, lParam);
}

static LRESULT CALLBACK MouseProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION && g_router) {
        auto* mi = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);
        // Inspect-mode click-to-look-point. Swallow the real DOWN (it would land at the frozen cursor)
        // and signal the tick, which fires a clean absolute click at the crosshair. Swallow the matching
        // real UP too. Our own injected click carries LLMHF_INJECTED, so it skips this and passes through.
        if (!(mi->flags & LLMHF_INJECTED)) {
            bool lDown = (wParam == WM_LBUTTONDOWN), rDown = (wParam == WM_RBUTTONDOWN);
            bool lUp   = (wParam == WM_LBUTTONUP),   rUp   = (wParam == WM_RBUTTONUP);
            if (g_router->state().inspectActive.load(std::memory_order_relaxed) && (lDown || rDown)) {
                g_router->state().commitButton.store(lDown ? 1 : 2, std::memory_order_relaxed);
                g_commitBtn = lDown ? 1 : 2;
                return 1;   // eat the real DOWN
            }
            if (g_commitBtn == 1 && lUp) { g_commitBtn = 0; return 1; }   // eat the matching real UP
            if (g_commitBtn == 2 && rUp) { g_commitBtn = 0; return 1; }
        }
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
    HMODULE hmod = GetModuleHandleW(nullptr);
    g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, MouseProc, hmod, 0);
    // Keyboard hook shares this thread (keystrokes are far rarer than mouse moves, so it adds no
    // meaningful latency to the mouse path). It swallows keyboard zoom/recenter binds. Best-effort:
    // mouse-hook success still gates start()'s overall result; a missing keyboard hook just falls
    // back to GetAsyncKeyState polling (no swallowing) via kbHookActive().
    g_kbHook    = SetWindowsHookExW(WH_KEYBOARD_LL, KbProc, hmod, 0);
    g_hookOk = (g_mouseHook != nullptr);
    g_kbOk   = (g_kbHook != nullptr);
    SetEvent(g_hookReady);                                        // publish the install result to start()
    if (!g_mouseHook) { if (g_kbHook) { UnhookWindowsHookEx(g_kbHook); g_kbHook = nullptr; } return 1; }
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {                // WM_QUIT (posted by stop()) ends this
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    UnhookWindowsHookEx(g_mouseHook);                            // unhook on the installing thread
    g_mouseHook = nullptr;
    if (g_kbHook) { UnhookWindowsHookEx(g_kbHook); g_kbHook = nullptr; }
    return 0;
}

bool InputRouter::start(int inButtonId, int inButtonId2, int outButtonId, int outButtonId2, bool swallow) {
    g_router = this;
    inButtonId_.store(inButtonId, std::memory_order_relaxed);
    inButtonId2_.store(inButtonId2, std::memory_order_relaxed);
    outButtonId_.store(outButtonId, std::memory_order_relaxed);
    outButtonId2_.store(outButtonId2, std::memory_order_relaxed);
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
    hookActive_.store(g_hookOk);     // hook is now the sole button-state authority (see hookActive())
    kbHookActive_.store(g_kbOk);     // keyboard hook is the authority for bound-key state (see kbHookActive())
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
// Keyboard analogue of ReleaseSwallowedButtons: synthesize a KEYUP for any bound key whose DOWN we
// swallowed but whose UP we never passed through, so teardown mid-press can't leave any consumer
// believing the key is held. A lone keyup with no matching down is harmless (apps ignore it).
static void ReleaseSwallowedKeys() {
    for (int vk = 0; vk < 256; ++vk) {
        if (!g_kbSwallowedDown[vk].exchange(false)) continue;
        INPUT in{};
        in.type = INPUT_KEYBOARD;
        in.ki.wVk = static_cast<WORD>(vk);
        in.ki.dwFlags = KEYEVENTF_KEYUP;
        SendInput(1, &in, sizeof(in));
        g_kbPressed[vk].store(false);
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
    ReleaseSwallowedKeys();      // ...nor a swallowed keyboard bind
    g_commitBtn = 0;             // clear the inspect click-swallow latch (the click is atomic; nothing held)
    hookActive_.store(false);
    kbHookActive_.store(false);
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
