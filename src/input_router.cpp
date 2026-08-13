#include "input_router.h"
#include "config.h"     // IsForbiddenBindVk (keyboard-bind safety blocklist)
#include "logging.h"    // hook-watchdog events (issue #156)
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
// point instead. g_commitDown[btn] remembers THAT button's swallowed DOWN so only its own matching UP is
// swallowed - per-button, so a left+right chord can't strand a stray UP. Atomic because stop() clears it
// off the tick thread. The tick's injected click carries LLMHF_INJECTED, so it is not re-swallowed.
static std::atomic<bool> g_commitDown[3] = {};   // index 1=left, 2=right; [0] unused

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
// Raw Input safety net for a key whose UP the hook never saw (issue #167) - see the WM_INPUT
// handler in main.cpp for why. Clearing BOTH records is the point: g_kbPressed unsticks the held
// state main reads, and g_kbSwallowedDown stops a later, unrelated UP from being swallowed on the
// strength of a DOWN whose UP already went past us. Idempotent with the hook's own clear.
void InputRouter::rawKeyUp(int vk) {
    if (vk <= 0 || vk > 255) return;
    // Cross-thread reordering guard: WM_INPUT events are drained up to a tick late, so a raw UP
    // from a fast release-press can be processed AFTER the live hook already recorded the NEXT
    // press's DOWN - clearing here would then cancel a hold that is physically down (and wipe the
    // swallow record, leaking the eventual real UP to the focused app). While the hook is alive it
    // delivers UPs itself, so the net is only needed when the hook is gone or stalled; skip the
    // clear when the hook saw a DOWN for this key within the last ~30 ms (auto-repeat keeps the
    // stamp fresh through a real hold; an evicted hook stops stamping, so the net still fires).
    if (kbHookActive() &&
        GetTickCount64() - kbLastHookDownMs_[vk].load(std::memory_order_relaxed) < 30) return;
    g_kbPressed[vk].store(false, std::memory_order_relaxed);
    g_kbSwallowedDown[vk].store(false, std::memory_order_relaxed);
}
void InputRouter::rawButtonUp(int xbuttonId) {
    if (xbuttonId != 1 && xbuttonId != 2) return;
    // Same reordering guard as rawKeyUp: no auto-repeat exists for a side-button, so a stale raw
    // UP landing after the hook's next DOWN would silently end a zoom hold until re-pressed.
    if (hookActive() &&
        GetTickCount64() - btnLastHookDownMs_[xbuttonId].load(std::memory_order_relaxed) < 30) return;
    setButtonState(xbuttonId, false);
}
void InputRouter::noteHookKeyDown(int vk) {
    if (vk > 0 && vk < 256) kbLastHookDownMs_[vk].store(GetTickCount64(), std::memory_order_relaxed);
}
void InputRouter::noteHookButtonDown(int xbuttonId) {
    if (xbuttonId == 1 || xbuttonId == 2)
        btnLastHookDownMs_[xbuttonId].store(GetTickCount64(), std::memory_order_relaxed);
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
        // Magnify model: our own injected Win+Plus/Minus chords must never be swallowed or
        // tracked (NumPad +/- are bindable zoom keys; swallowing our own injection would both
        // starve Magnifier and feed back as a phantom zoom press). Gated on the model so
        // injected keys from other tools keep working normally under the render model.
        if ((ks->flags & LLKHF_INJECTED) && g_router->ignoreInjectedKeys())
            return CallNextHookEx(g_kbHook, code, wParam, lParam);
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
                g_router->noteHookKeyDown(vk);   // recency guard for the raw UP safety net
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
        // Pan coherence (issue #195): signal the tick thread the instant the pointer actually
        // moves. Native Magnifier writes its magnification offset from inside its own mouse
        // hook, so the offset it publishes is paired with exactly the cursor sample that event
        // produced. A free-running poll instead pairs each offset with a sample of RANDOM
        // staleness - variable lead, which is what the eye reads as wobble (measured: raising
        // the poll rate to ~300/s changed nothing, it only re-rolled the dice faster). The
        // Magnification API is thread-affine, so the write itself must stay on the tick thread:
        // this event wakes it immediately instead. Injected moves are skipped - our own
        // SetCursorPos must never drive a repan (that is the weld tug-of-war, self-inflicted).
        // Injected moves signal too: the only consumer (TransformModel::fastCursorRepan) runs
        // exclusively in FREE-cursor mode, where we never SetCursorPos - so there is no weld
        // for our own warps to fight, and a measurement harness that drives the pointer with
        // SendInput exercises exactly the path a real hand does (issue #195 wobble probe).
        if (wParam == WM_MOUSEMOVE) {
            g_router->state().moveSignals.fetch_add(1, std::memory_order_relaxed);
            HANDLE mv = (HANDLE)g_router->mouseMoveEvent();
            if (mv) SetEvent(mv);
        }
        // Inspect-mode click-to-look-point. Swallow the real DOWN (it would land at the frozen cursor)
        // and signal the tick, which fires a clean absolute click at the crosshair. Swallow the matching
        // real UP too. Our own injected click carries LLMHF_INJECTED, so it skips this and passes through.
        if (!(mi->flags & LLMHF_INJECTED)) {
            int cDown = (wParam == WM_LBUTTONDOWN) ? 1 : (wParam == WM_RBUTTONDOWN) ? 2 : 0;
            int cUp   = (wParam == WM_LBUTTONUP)   ? 1 : (wParam == WM_RBUTTONUP)   ? 2 : 0;
            if (cDown && g_router->state().inspectActive.load(std::memory_order_relaxed)) {
                g_commitDown[cDown].store(true, std::memory_order_relaxed);   // remember THIS button's DOWN
                // Count the click (don't overwrite) so a fast second click before the tick drains isn't lost.
                auto& pending = (cDown == 1) ? g_router->state().commitLeft : g_router->state().commitRight;
                pending.fetch_add(1, std::memory_order_relaxed);
                return 1;   // eat the real DOWN; the tick fires the click at the look point
            }
            // Swallow an UP iff THIS button's DOWN was swallowed (per-button, so a chord never strands one).
            if (cUp && g_commitDown[cUp].exchange(false, std::memory_order_relaxed)) return 1;
        }
        // Diagnostics (issue #113): count every side-button transition the hook observes, including
        // WM_XBUTTONDBLCLK (which replaces the 2nd DOWN of a fast double-press and is otherwise ignored
        // by the held-state logic). Counters only - no I/O in the hook. The tick thread logs them.
        if (wParam == WM_XBUTTONDOWN || wParam == WM_XBUTTONUP || wParam == WM_XBUTTONDBLCLK) {
            WORD hi = HIWORD(mi->mouseData);
            int bid = (hi == XBUTTON1) ? 1 : (hi == XBUTTON2 ? 2 : 0);
            if (bid) {
                auto& st = g_router->state();
                if (wParam == WM_XBUTTONDOWN)      st.dbgHookDown[bid].fetch_add(1, std::memory_order_relaxed);
                else if (wParam == WM_XBUTTONUP)   st.dbgHookUp[bid].fetch_add(1, std::memory_order_relaxed);
                else                               st.dbgHookDbl[bid].fetch_add(1, std::memory_order_relaxed);
            }
        }
        int id = xbuttonIdFromHook(wParam, lParam);
        bool down = (wParam == WM_XBUTTONDOWN);
        bool up   = (wParam == WM_XBUTTONUP);
        if (id != 0 && (down || up)) {
            if (down) g_router->noteHookButtonDown(id);   // recency guard for the raw UP safety net
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
// Tick thread -> hook thread: set the keyboard hook's installed state. wParam != 0 installs
// (watchdog recovery, or leaving a game), wParam == 0 uninstalls (entering a game). A low-level
// hook is bound to the message queue of the thread that installs it, so both must happen there.
static constexpr UINT kMsgSetKbHook = WM_APP + 11;

static DWORD WINAPI HookThreadProc(LPVOID) {
    MSG msg;
    PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);   // force a message queue to exist
    // A low-level hook callback must complete within LowLevelHooksTimeout or Windows silently
    // evicts the hook (issue #156). The callbacks here are already atomics-only, but they cannot
    // run at all while this thread is descheduled, and a game's launch load spike does exactly
    // that - on the reporting machine the timeout was 300 ms and the keyboard hook died every
    // time a heavily modded RDR2 was launched with Wind already running. Raising the priority is
    // the documented mitigation for a dedicated hook thread: it does no work besides servicing
    // the hooks, so it can never starve anything else, and it stops the whole system's input
    // waiting on us (a late hook thread delays input for EVERY process, not just ours).
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
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
        // Watchdog recovery: re-install the keyboard hook Windows evicted from under us. Must run
        // HERE - a low-level hook is bound to the message queue of the thread that installs it, so
        // installing from the tick thread would produce a hook nothing ever pumps.
        if (msg.message == kMsgSetKbHook) {
            const bool want = msg.wParam != 0;
            if (g_kbHook) { UnhookWindowsHookEx(g_kbHook); g_kbHook = nullptr; }   // may already be gone
            // Per-key records are stale across the gap either way: an eviction means the matching UP
            // was never seen, and a deliberate uninstall means later UPs arrive unhooked. Clear them
            // rather than let a stale record eat an unrelated UP later (stuck key). No synthetic UP
            // is needed: we only ever swallow our OWN binds, so no other app saw the DOWN.
            for (int vk = 0; vk < 256; ++vk) { g_kbSwallowedDown[vk].store(false); g_kbPressed[vk].store(false); }
            if (want) g_kbHook = SetWindowsHookExW(WH_KEYBOARD_LL, KbProc, hmod, 0);
            if (g_router) g_router->onKbHookStateChanged(want && g_kbHook != nullptr);
            continue;
        }
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
    // Pan-coherence wake event (issue #195). Auto-reset: the tick thread consumes one signal
    // per real pointer move. Created before the hook thread so the hook never races a null.
    if (!mouseMoveEvent_) mouseMoveEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
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
// Watchdog entry points (issue #156). See the header for why an evicted hook is invisible.
void InputRouter::requestKbHookReinstall() {
    if (!kbHookWanted_.load(std::memory_order_relaxed)) return;   // deliberately suspended, not dead
    // The hook we are replacing is dead, so anything it recorded as held can never be released by
    // it (issue #167). Drop those records before the new hook goes in, or recovery inherits a
    // phantom hold and the zoom runs away the moment the hook is authoritative again.
    ReleaseSwallowedKeys();
    // Drop the authority claim FIRST so main's keyDown falls back to GetAsyncKeyState on the very
    // next tick. A dead hook swallows nothing, so polling sees the real key state and the binds
    // work again immediately - the re-install below only restores swallowing.
    kbHookActive_.store(false, std::memory_order_relaxed);
    kbHookRecovering_.store(true, std::memory_order_relaxed);
    if (g_hookThreadId) PostThreadMessageW(g_hookThreadId, kMsgSetKbHook, 1, 0);
}

void InputRouter::setKeyboardHookWanted(bool want) {
    if (kbHookWanted_.exchange(want, std::memory_order_relaxed) == want) return;   // no change
    // Stop claiming authority the moment we ask for a suspend, so the very next tick already polls
    // (binds keep working: with the hook gone nothing swallows them, so GetAsyncKeyState is right).
    // Release whatever it was holding at the same time (issue #167): suspending mid-press leaves a
    // DOWN the hook will never see the UP for, and that record survives to poison the next resume.
    if (!want) {
        kbHookActive_.store(false, std::memory_order_relaxed);
        ReleaseSwallowedKeys();
    }
    if (g_hookThreadId) PostThreadMessageW(g_hookThreadId, kMsgSetKbHook, want ? 1 : 0, 0);
}

void InputRouter::onKbHookStateChanged(bool active) {
    kbHookActive_.store(active, std::memory_order_relaxed);
    const bool recovering = kbHookRecovering_.exchange(false, std::memory_order_relaxed);
    if (!kbHookWanted_.load(std::memory_order_relaxed)) {
        wind::Log(wind::LogLevel::Info, "input",
                  "keyboard hook SUSPENDED (fullscreen game foreground); binds poll instead");
    } else if (active && recovering) {
        unsigned n = kbHookReinstalls_.fetch_add(1, std::memory_order_relaxed) + 1;
        wind::Log(wind::LogLevel::Warn, "input",
                  "keyboard hook was evicted by Windows; re-installed (recovery #%u)", n);
    } else if (active) {
        wind::Log(wind::LogLevel::Info, "input", "keyboard hook resumed (no fullscreen game foreground)");
    } else {
        // Left false: main keeps polling, which still works (nothing is swallowing). The next
        // detected divergence retries, so a transient failure heals on the following press.
        wind::Log(wind::LogLevel::Error, "input",
                  "keyboard hook install FAILED gle=%lu; polling fallback stays active",
                  (unsigned long)GetLastError());
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
    for (auto& d : g_commitDown) d.store(false, std::memory_order_relaxed);   // clear inspect click latches
    hookActive_.store(false);
    kbHookActive_.store(false);
    g_router = nullptr;   // cleared BEFORE closing the event: the hook reads it through g_router
    if (mouseMoveEvent_) { CloseHandle(mouseMoveEvent_); mouseMoveEvent_ = nullptr; }
}
void InputRouter::drainRaw(int& dx, int& dy) {
    dx = state_.rawDx.exchange(0);
    dy = state_.rawDy.exchange(0);
}

// Per-packet ballistic cooking for the Inspect-mode pan. Only needed while Inspect is on (the OS
// cursor is frozen then, so raw mickeys drive the look point); skipped otherwise so it costs nothing
// in the normal path. Windows accelerates per packet on each packet's magnitude, so cook here (one
// WM_INPUT = one packet) and accumulate the sub-pixel result; the tick drains it via drainCooked.
void InputRouter::cookPacket(int dx, int dy) {
    if (!state_.inspectActive.load(std::memory_order_relaxed)) return;
    double cx, cy;
    CookMickeyPacket(ballistics_, dx, dy, cx, cy);
    cookedX_ += cx;
    cookedY_ += cy;
}

void AccumulateRaw(InputRouter& r, int dx, int dy) {
    r.state().rawDx.fetch_add(dx);
    r.state().rawDy.fetch_add(dy);
    r.cookPacket(dx, dy);   // Inspect-mode speed match (no-op unless Inspect is active)
}
}
