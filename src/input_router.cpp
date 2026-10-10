#include "input_router.h"
#include "config.h"     // IsForbiddenBindVk (keyboard-bind safety blocklist)
#include "pointer_binds.h" // button/wheel bind matching, the mask keystroke (#285)
#include "swallow_ledger.h" // the swallowed-DOWN records (pure, tested)
#include "logging.h"    // hook-watchdog events (issue #156)
#include "typing_key.h" // the typing-key stamp for the click quiet period (#328)
#include "event_order.h" // raw-UP reordering guard on event times
#include <windows.h>
#include <atomic>
namespace wind {
static InputRouter* g_router = nullptr;
static HHOOK   g_mouseHook    = nullptr;
static HHOOK   g_kbHook       = nullptr;   // WH_KEYBOARD_LL, shares g_hookThread with the mouse hook
static bool    g_kbOk         = false;     // result of the keyboard SetWindowsHookExW, via g_hookReady
static TypingKeyFilter g_typingKeys;       // hook thread only: fresh non-modifier downs (#328)
// Per-VK keyboard state (index = Virtual-Key code, 0..255). Touched by the hook thread (KbProc), the
// tick thread (setKeys / main's keyPressed reads), and teardown (ReleaseSwallowedKeys via stop()),
// so they must be atomic. g_kbPressed = physical down-state (the authority while the hook is active,
// since a swallowed key never appears in GetAsyncKeyState). g_kbSwallowedDown = whether we swallowed
// the DOWN, so only the matching UP is swallowed too (keeps the system's down/up view balanced and a
// key can never be left believed-held).
static std::atomic<bool> g_kbPressed[256]      = {};
static wind::SwallowLedger<256> g_kbSwallowedDown;
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
static wind::SwallowLedger<6> g_swallowedDown;   // index = button id 1..5 (#285: 3/4/5 = L/R/M)
// Which direction a pressed bound button is holding (1 in, 2 out, 0 none) and that bind's modifiers,
// per button id. The directions' held flags are derived from these, so two buttons on the same
// direction (a side button and Ctrl+Alt+click) can never release each other.
static std::atomic<int> g_btnDir[6] = {};
static std::atomic<int> g_btnMods[6] = {};
static WheelAccum g_wheelAcc;   // hook thread only
// Event-driven idle (#71): wakes the sleeping main loop. SetEvent is a cheap, lock-free kernel call,
// safe from the LL hooks; it is only raised on edges (never per mouse move or per unbound key), and
// always AFTER the state the main loop will read has been published.
static HANDLE g_idleWake = nullptr;
static inline void WakeMain() { if (g_idleWake) SetEvent(g_idleWake); }
// Zoom timeline (#310): the first press since the main loop last took it. One QPC read and a CAS.
static std::atomic<long long> g_pressQpc{0};
static inline void StampPress() {
    LARGE_INTEGER q; QueryPerformanceCounter(&q);
    long long zero = 0;
    g_pressQpc.compare_exchange_strong(zero, q.QuadPart, std::memory_order_relaxed);
}
// One mask keystroke per Alt/Win hold is enough (#301): set when injected, cleared when the keyboard
// hook sees Alt or Win released. Without it a spun wheel injected two key events per notch.
static std::atomic<bool> g_maskedThisHold{false};
// PublishButtonHeld runs on the hook, tick and main threads; unserialised, a stale recompute could
// land after a newer one and strand a direction as held (#301).
static std::atomic_flag g_publishLock = ATOMIC_FLAG_INIT;

// The modifiers held right now (bit 1 Ctrl, 2 Alt, 4 Shift, 8 Win). Modifiers are never swallowed,
// so the async state is current for them even inside the hook.
static int HeldModsNow() {
    int m = 0;
    if (GetAsyncKeyState(VK_CONTROL) & 0x8000) m |= kModCtrl;
    if (GetAsyncKeyState(VK_MENU) & 0x8000)    m |= kModAlt;
    if (GetAsyncKeyState(VK_SHIFT) & 0x8000)   m |= kModShift;
    if ((GetAsyncKeyState(VK_LWIN) & 0x8000) || (GetAsyncKeyState(VK_RWIN) & 0x8000)) m |= kModWin;
    return m;
}
// One unassigned keystroke so a held Alt/Win is not released "alone" after we swallowed the event
// in between (Start menu / menu-bar activation, see NeedsMaskKey). Injected: our hooks pass it.
static void InjectMaskKey() {
    // While the keyboard hook is suspended nothing would clear the once-per-hold flag, so mask
    // every time then (the old behaviour).
    if (g_router && g_router->kbHookActive() && g_maskedThisHold.exchange(true)) return;
    INPUT in[2]{};
    in[0].type = INPUT_KEYBOARD; in[0].ki.wVk = (WORD)kMaskVk; in[0].ki.dwExtraInfo = (ULONG_PTR)kWindInjectTag;
    in[1] = in[0]; in[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, in, sizeof(INPUT));
}
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

bool InputRouter::matchButton(int button, int heldMods, int& dir, int& mods) const {
    const ButtonSlot slots[4] = {
        { inButtonId_.load(std::memory_order_relaxed),   inButtonMods_.load(std::memory_order_relaxed),   1 },
        { inButtonId2_.load(std::memory_order_relaxed),  inButtonMods2_.load(std::memory_order_relaxed),  1 },
        { outButtonId_.load(std::memory_order_relaxed),  outButtonMods_.load(std::memory_order_relaxed),  2 },
        { outButtonId2_.load(std::memory_order_relaxed), outButtonMods2_.load(std::memory_order_relaxed), 2 },
    };
    const int i = PickButtonSlot(slots, 4, button, heldMods);
    if (i < 0) return false;
    dir = slots[i].dir; mods = slots[i].mods;
    return true;
}
// Recompute the per-direction held flags from the per-button records.
static void PublishButtonHeld(InputState& st) {
    while (g_publishLock.test_and_set(std::memory_order_acquire)) {}   // held for a few loads
    bool in = false, out = false; int inM = 0, outM = 0;
    for (int b = 1; b <= 5; ++b) {
        const int d = g_btnDir[b].load(std::memory_order_relaxed);
        if (d == 1) { in = true;  inM  |= g_btnMods[b].load(std::memory_order_relaxed); }
        if (d == 2) { out = true; outM |= g_btnMods[b].load(std::memory_order_relaxed); }
    }
    st.inHeldMods.store(inM, std::memory_order_relaxed);
    st.outHeldMods.store(outM, std::memory_order_relaxed);
    const bool wasAny = st.inHeld.load() || st.outHeld.load();
    const bool changed = st.inHeld.load() != in || st.outHeld.load() != out;
    if (!wasAny && (in || out)) StampPress();   // a hold just started
    st.inHeld.store(in);
    st.outHeld.store(out);
    g_publishLock.clear(std::memory_order_release);
    if (changed) WakeMain();   // every button/click bind press and release, and a rebind that drops a hold
}
// Shared by the WH_MOUSE_LL hook (below) and main's WM_INPUT path: map a button to held. A press
// holds the direction of the slot it matches (with the modifiers held now); a release frees it.
void InputRouter::setButtonState(int buttonId, bool down) {
    if (buttonId < 1 || buttonId > 5) return;
    if (down) {
        int dir = 0, mods = 0;
        if (!matchButton(buttonId, HeldModsNow(), dir, mods)) return;
        g_btnMods[buttonId].store(mods, std::memory_order_relaxed);
        g_btnDir[buttonId].store(dir, std::memory_order_relaxed);
    } else {
        g_btnDir[buttonId].store(0, std::memory_order_relaxed);
    }
    PublishButtonHeld(state_);
}
bool InputRouter::isZoomButton(int xbuttonId) const {
    return xbuttonId == inButtonId_.load(std::memory_order_relaxed)
        || xbuttonId == inButtonId2_.load(std::memory_order_relaxed)
        || xbuttonId == outButtonId_.load(std::memory_order_relaxed)
        || xbuttonId == outButtonId2_.load(std::memory_order_relaxed);
}
void InputRouter::setButtons(int inButtonId, int inButtonId2, int outButtonId, int outButtonId2) {
    setButtonBinds(inButtonId, 0, inButtonId2, 0, outButtonId, 0, outButtonId2, 0);
}
void InputRouter::setButtonBinds(int inButtonId, int inMods, int inButtonId2, int inMods2,
                                 int outButtonId, int outMods, int outButtonId2, int outMods2) {
    inButtonMods_.store(inMods, std::memory_order_relaxed);
    inButtonMods2_.store(inMods2, std::memory_order_relaxed);
    outButtonMods_.store(outMods, std::memory_order_relaxed);
    outButtonMods2_.store(outMods2, std::memory_order_relaxed);
    inButtonId_.store(inButtonId, std::memory_order_relaxed);
    inButtonId2_.store(inButtonId2, std::memory_order_relaxed);
    outButtonId_.store(outButtonId, std::memory_order_relaxed);
    outButtonId2_.store(outButtonId2, std::memory_order_relaxed);
    // Clear any held state from the previous mapping (else a press of the OLD button that was in
    // progress would never get its UP event matched and inHeld/outHeld would stick true).
    for (int b = 1; b <= 5; ++b) g_btnDir[b].store(0, std::memory_order_relaxed);
    PublishButtonHeld(state_);
    // The swallowed-DOWN records are NOT cleared (#301): an UP whose DOWN we swallowed must still
    // be swallowed after a remap mid-press, or the app sees a lone button-up (a side button's
    // XBUTTONUP alone is browser Back/Forward). Each record clears on its own UP.
}

bool InputRouter::isBoundKey(int vk) const {
    if (vk <= 0 || vk > 255 || IsForbiddenBindVk(vk)) return false;   // never track/swallow forbidden keys
    return vk == kbZoomInVk_.load(std::memory_order_relaxed)
        || vk == kbZoomInVk2_.load(std::memory_order_relaxed)
        || vk == kbZoomOutVk_.load(std::memory_order_relaxed)
        || vk == kbZoomOutVk2_.load(std::memory_order_relaxed)
        || vk == kbRecenterVk_.load(std::memory_order_relaxed)
        || vk == kbCursorLockVk_.load(std::memory_order_relaxed)
        || vk == panVk_[0].load(std::memory_order_relaxed) || vk == panVk_[1].load(std::memory_order_relaxed)
        || vk == panVk_[2].load(std::memory_order_relaxed) || vk == panVk_[3].load(std::memory_order_relaxed);
}
void InputRouter::setPanKeys(const int vk[4], const int mods[4]) {
    // Only the old and new pan keys' pressed records are reset, so a held zoom key survives a pan
    // rebind. Swallow records are kept: a swallowed DOWN must still get its UP swallowed (#301).
    for (int i = 0; i < 4; ++i) {
        const int old = panVk_[i].load(std::memory_order_relaxed);
        if (old > 0 && old < 256) g_kbPressed[old].store(false);
        if (vk[i] > 0 && vk[i] < 256) g_kbPressed[vk[i]].store(false);
        panMods_[i].store(mods[i], std::memory_order_relaxed);
        panVk_[i].store(vk[i], std::memory_order_relaxed);
        panPresses_[i].store(0, std::memory_order_relaxed);
    }
}
void InputRouter::notePanPress(int vk) {
    if (!panArmed_.load(std::memory_order_relaxed)) return;
    for (int i = 0; i < 4; ++i)
        if (vk == panVk_[i].load(std::memory_order_relaxed)) panPresses_[i].fetch_add(1, std::memory_order_relaxed);
}
bool InputRouter::keySwallowed(int vk) const {
    if (vk <= 0 || vk > 255) return false;
    return g_kbSwallowedDown.isSet(vk);
}
void InputRouter::setKeyMods(int zoomInMods, int zoomInMods2, int zoomOutMods, int zoomOutMods2,
                             int recenterMods, int cursorLockMods) {
    kbRecenterMods_.store(recenterMods, std::memory_order_relaxed);
    kbCursorLockMods_.store(cursorLockMods, std::memory_order_relaxed);
    kbZoomInMods_.store(zoomInMods, std::memory_order_relaxed);
    kbZoomInMods2_.store(zoomInMods2, std::memory_order_relaxed);
    kbZoomOutMods_.store(zoomOutMods, std::memory_order_relaxed);
    kbZoomOutMods2_.store(zoomOutMods2, std::memory_order_relaxed);
}
bool InputRouter::keyBindMatches(int vk, int heldMods) const {
    if (!isBoundKey(vk)) return false;
    auto slot = [&](const std::atomic<int>& v, const std::atomic<int>& m) {
        return vk == v.load(std::memory_order_relaxed) && ModsSatisfied(m.load(std::memory_order_relaxed), heldMods);
    };
    return slot(kbZoomInVk_, kbZoomInMods_) || slot(kbZoomInVk2_, kbZoomInMods2_)
        || slot(kbZoomOutVk_, kbZoomOutMods_) || slot(kbZoomOutVk2_, kbZoomOutMods2_)
        || slot(kbRecenterVk_, kbRecenterMods_) || slot(kbCursorLockVk_, kbCursorLockMods_)
        || (panArmed_.load(std::memory_order_relaxed) &&
            (slot(panVk_[0], panMods_[0]) || slot(panVk_[1], panMods_[1]) ||
             slot(panVk_[2], panMods_[2]) || slot(panVk_[3], panMods_[3])));
}
bool InputRouter::keyPressed(int vk) const {
    if (vk <= 0 || vk > 255) return false;
    return g_kbPressed[vk].load(std::memory_order_relaxed);
}
// Raw Input safety net for a key whose UP the hook never saw (issue #167) - see the WM_INPUT
// handler in main.cpp for why. Clearing BOTH records is the point: g_kbPressed unsticks the held
// state main reads, and g_kbSwallowedDown stops a later, unrelated UP from being swallowed on the
// strength of a DOWN whose UP already went past us. Idempotent with the hook's own clear.
void InputRouter::rawKeyUp(int vk, uint32_t eventTimeMs) {
    if (vk <= 0 || vk > 255) return;
    // Cross-thread reordering guard: WM_INPUT events are drained up to a tick late, so a raw UP
    // from a fast release-press can be processed AFTER the live hook already recorded the NEXT
    // press's DOWN - clearing here would then cancel a hold that is physically down (and wipe the
    // swallow record, leaking the eventual real UP to the focused app). While the hook is alive it
    // delivers UPs itself, so the net is only needed when the hook is gone or stalled; skip the
    // clear when the hook saw a DOWN for this key AFTER this UP (event times, not the wall clock: a
    // main-thread stall of any length cannot defeat it; auto-repeat keeps the stamp current through
    // a real hold; an evicted hook stops stamping, so the net still fires).
    if (kbHookActive() && RawUpIsStale(kbLastHookDownMs_[vk].load(std::memory_order_relaxed), eventTimeMs)) return;
    g_kbPressed[vk].store(false, std::memory_order_relaxed);
    g_kbSwallowedDown.clear(vk);
}
void InputRouter::rawButtonUp(int xbuttonId, uint32_t eventTimeMs) {
    if (xbuttonId < 1 || xbuttonId > 5) return;
    // Left/right/middle (3-5) only matter while one of them holds a zoom; every ordinary click
    // passes straight through here.
    if (xbuttonId >= 3 && g_btnDir[xbuttonId].load(std::memory_order_relaxed) == 0) return;
    // Same reordering guard as rawKeyUp: no auto-repeat exists for a side-button, so a stale raw
    // UP landing after the hook's next DOWN would silently end a zoom hold until re-pressed.
    if (hookActive() && RawUpIsStale(btnLastHookDownMs_[xbuttonId].load(std::memory_order_relaxed), eventTimeMs)) return;
    setButtonState(xbuttonId, false);
}
void InputRouter::noteHookKeyDown(int vk, uint32_t eventTimeMs) {
    if (vk > 0 && vk < 256) kbLastHookDownMs_[vk].store(PackEventStamp(eventTimeMs), std::memory_order_relaxed);
}
void InputRouter::noteHookButtonDown(int xbuttonId, uint32_t eventTimeMs) {
    if (xbuttonId >= 1 && xbuttonId <= 5)
        btnLastHookDownMs_[xbuttonId].store(PackEventStamp(eventTimeMs), std::memory_order_relaxed);
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
    for (int i = 0; i < 256; ++i) { g_kbPressed[i].store(false); g_kbSwallowedDown.clear(i); }
}

static LRESULT CALLBACK KbProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION && g_router) {
        auto* ks = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
        int vk = static_cast<int>(ks->vkCode);
        bool down = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
        bool up   = (wParam == WM_KEYUP   || wParam == WM_SYSKEYUP);
        // Alt or Win released: the next hold gets its own mask keystroke.
        if (up && (vk == VK_LMENU || vk == VK_RMENU || vk == VK_MENU || vk == VK_LWIN || vk == VK_RWIN))
            g_maskedThisHold.store(false, std::memory_order_relaxed);
        // Any key activity, down OR up: tracking's keyboard gate (#289). Ups count so a focus change
        // committed by a release (Alt+Tab held for a while) is still keyboard-driven (review).
        // Wind's own mask keystroke (VK 0xE8) is not a user key: it must not stamp the clock.
        if ((down || up) && vk != kMaskVk) g_router->noteAnyKeyDown(GetTickCount64());
        // Typing (#328): only a fresh non-modifier down ends the click quiet period.
        if ((down || up) && g_typingKeys.note(vk, down)) g_router->noteTypingKeyDown(GetTickCount64());
        // Only bound (non-forbidden) keys are tracked/swallowed; every other keystroke passes through
        // untouched. isBoundKey already range-checks vk and excludes IsForbiddenBindVk keys.
        if ((down || up) && g_router->isBoundKey(vk)) {
            bool swallow = false;
            if (down) {
                // Auto-repeat re-fires WM_KEYDOWN. main reads g_kbPressed as the physical down-state
                // and does its own rising-edge work for taps.
                const bool firstDown = !g_kbPressed[vk].exchange(true);
                g_router->noteHookKeyDown(vk, ks->time);   // event-time guard for the raw UP safety net
                if (firstDown) { StampPress(); WakeMain(); }   // edges only: auto-repeat never wakes the loop (#71)
                if (firstDown) {
                    // Decide ONCE per press: swallow only if a bind on this key has all its modifiers
                    // held now (#285: Ctrl+F1 must not eat a plain F1). Auto-repeat then follows that
                    // decision, so a key the app already saw going down is never swallowed mid-press.
                    const int held = HeldModsNow();
                    if (g_router->swallowEnabled() && g_router->keyBindMatches(vk, held)) {
                        g_kbSwallowedDown.markDown(vk);
                        g_router->notePanPress(vk);   // a tap between two tick samples still nudges
                        // Alt or Win held: mask it so its release is not a lone tap (Start / menu bar).
                        if (NeedsMaskKey(held)) InjectMaskKey();
                        swallow = true;
                    } else {
                        g_kbSwallowedDown.clear(vk);   // a stale record must not eat this press's UP
                    }
                } else {
                    swallow = g_kbSwallowedDown.isSet(vk);
                }
            } else { // up: swallow iff we swallowed its DOWN, so the system's down/up view stays balanced.
                g_kbPressed[vk].store(false);
                WakeMain();
                if (g_kbSwallowedDown.consumeUp(vk)) swallow = true;
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
        // Click zoom binds (#285): left/right/middle with modifiers. Checked before Inspect's click
        // routing, so a bound Ctrl+Alt+click zooms even in Inspect. Wind's OWN injections (tagged:
        // Inspect's clicks, the native-Magnifier notches) are never binds; other injectors are.
        if ((unsigned long long)mi->dwExtraInfo != kWindInjectTag) {
            int cb = 0; bool cbDown = false, cbUp = false;
            switch (wParam) {
                case WM_LBUTTONDOWN: cb = 3; cbDown = true; break;
                case WM_LBUTTONUP:   cb = 3; cbUp = true;   break;
                case WM_RBUTTONDOWN: cb = 4; cbDown = true; break;
                case WM_RBUTTONUP:   cb = 4; cbUp = true;   break;
                case WM_MBUTTONDOWN: cb = 5; cbDown = true; break;
                case WM_MBUTTONUP:   cb = 5; cbUp = true;   break;
                default: break;
            }
            if (cb && cbDown) {
                const int held = HeldModsNow();
                int dir = 0, mods = 0;
                if (g_router->matchButton(cb, held, dir, mods)) {
                    g_router->noteHookButtonDown(cb, mi->time);   // event-time guard for the raw UP safety net
                    g_btnMods[cb].store(mods, std::memory_order_relaxed);
                    g_btnDir[cb].store(dir, std::memory_order_relaxed);
                    PublishButtonHeld(g_router->state());
                    if (g_router->swallowEnabled() && g_router->keyboardHookWanted()) {
                        g_swallowedDown.markDown(cb);
                        if (NeedsMaskKey(held)) InjectMaskKey();
                        return 1;
                    }
                }
                // Not swallowed: drop any record left by a DOWN whose UP never arrived, or this
                // click's UP would be eaten on its strength.
                g_swallowedDown.clear(cb);
            } else if (cb && cbUp) {
                if (g_btnDir[cb].exchange(0, std::memory_order_relaxed) != 0) PublishButtonHeld(g_router->state());
                // Balanced: swallow an up iff we swallowed its down, even if the modifiers were
                // released in between, so the app never sees a lone click-up.
                if (g_swallowedDown.consumeUp(cb)) return 1;
            }
            // Scroll-wheel zoom: a notch with the bound modifiers held zooms instead of scrolling.
            // Wheel up / down are zoom binds (#318, button codes 6/7 in the zoom slots); the legacy
            // zoomWheelMods form is still honoured when migration found no free slot.
            if (wParam == WM_MOUSEWHEEL) {
                const int delta = (short)HIWORD(mi->mouseData);
                const int wm = g_router->wheelMods();
                const int wcode = WheelCode(delta);
                int slotDir = 0, slotMods = 0;
                const int held = HeldModsNow();
                if (wcode != 0) g_router->matchButton(wcode, held, slotDir, slotMods);
                if (wcode != 0) {
                    const WheelDecision wd = DecideWheel(g_wheelAcc, delta, held, slotDir, wm);
                    if (wd.claimed) {
                        if (wd.zoomSteps) { StampPress(); g_router->state().wheelSteps.fetch_add(wd.zoomSteps, std::memory_order_relaxed); WakeMain(); }
                        // Pass-through apps (and swallow off) still zoom, and get the notch too.
                        if (g_router->swallowEnabled() && g_router->keyboardHookWanted()) {
                            if (NeedsMaskKey(held)) InjectMaskKey();
                            return 1;
                        }
                        return CallNextHookEx(g_mouseHook, code, wParam, lParam);
                    }
                }
            }
        }
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
            if (cDown) g_commitDown[cDown].store(false, std::memory_order_relaxed);   // stale record from a lost UP
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
            if (down) g_router->noteHookButtonDown(id, mi->time);   // event-time guard for the raw UP safety net
            g_router->setButtonState(id, down);
            bool swallow = false;
            if (down) {
                // Swallow the DOWN only if it matched a zoom bind now (modifiers included, #285);
                // remember it so the matching UP is swallowed too (balanced down/up view).
                if (g_router->swallowEnabled() && g_btnDir[id].load(std::memory_order_relaxed) != 0) {
                    g_swallowedDown.markDown(id);
                    swallow = true;
                    const int held = HeldModsNow();
                    if (NeedsMaskKey(held)) InjectMaskKey();
                }
            } else { // up: swallow iff we swallowed its DOWN. Never swallow an UP whose DOWN the
                     // system already saw - that is exactly what left the button stuck-down.
                if (g_swallowedDown.consumeUp(id)) {
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
    SetThreadDescription(GetCurrentThread(), L"Wind input hooks");   // names it in WPA (#361)
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
            for (int vk = 0; vk < 256; ++vk) { g_kbSwallowedDown.clear(vk); g_kbPressed[vk].store(false); }
            // The once-per-hold mask flag is stale across the gap too: the Alt/Win UP that clears it
            // may have been missed, which would suppress every later mask keystroke.
            g_maskedThisHold.store(false, std::memory_order_relaxed);
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
    if (!g_idleWake) g_idleWake = CreateEventW(nullptr, FALSE, FALSE, nullptr);   // before any hook exists (#71)
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
// Called when the hook is torn down (quit, shutdown racing a press).
// No button-up is synthesised (#301): a swallowed DOWN never reached the system, so nothing believes
// the button is held, and a lone synthesised UP had effects of its own (a right-click menu, browser
// Back/Forward, a drag finished in the wrong window). Only our own records are cleared.
static void ReleaseSwallowedButtons() {
    for (int id = 1; id <= 5; ++id) g_swallowedDown.clear(id);
}
// Keys differ from buttons: synthesize a KEYUP for any bound key whose DOWN we
// swallowed but whose UP we never passed through, so teardown mid-press can't leave any consumer
// believing the key is held. A lone keyup with no matching down is harmless (apps ignore it).
static void ReleaseSwallowedKeys() {
    for (int vk = 0; vk < 256; ++vk) {
        if (!g_kbSwallowedDown.consumeUp(vk)) continue;
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
    ReleaseSwallowedButtons();   // drop our swallow records; no synthetic button-up (#301)
    ReleaseSwallowedKeys();      // ...nor a swallowed keyboard bind
    for (auto& d : g_commitDown) d.store(false, std::memory_order_relaxed);   // clear inspect click latches
    hookActive_.store(false);
    kbHookActive_.store(false);
    g_router = nullptr;
    // After the hook thread has joined: nothing can signal it any more.
    if (g_idleWake) { CloseHandle(g_idleWake); g_idleWake = nullptr; }
}
void* InputRouter::wakeEvent() const { return g_idleWake; }
long long InputRouter::takePressQpc() { return g_pressQpc.exchange(0, std::memory_order_relaxed); }
long long InputRouter::peekPressQpc() const { return g_pressQpc.load(std::memory_order_relaxed); }
bool InputRouter::anyBoundKeyPressed() const {
    const int vks[] = { kbZoomInVk_.load(), kbZoomInVk2_.load(), kbZoomOutVk_.load(), kbZoomOutVk2_.load(),
                        kbRecenterVk_.load(), kbCursorLockVk_.load() };
    for (int vk : vks) if (vk > 0 && vk < 256 && g_kbPressed[vk].load(std::memory_order_relaxed)) return true;
    return false;
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
