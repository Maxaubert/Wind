#pragma once
#include <atomic>
#include "mouse_ballistics.h"   // BallisticsConfig (pure; Inspect-mode speed match)
namespace wind {
// Holds input state shared between the hook/raw-input callbacks and the tick thread.
struct InputState {
    std::atomic<int>  rawDx{0};      // summed since last drain
    std::atomic<int>  rawDy{0};
    std::atomic<bool> inHeld{false}; // zoom-in side button physically down
    std::atomic<bool> outHeld{false};
    // Inspect-mode click routing (tick <-> WH_MOUSE_LL hook). While Inspect is on the real cursor is
    // frozen elsewhere, so the hook swallows a real left/right click (it would land at the frozen point)
    // and hands the tick PER-BUTTON pending counts; the tick fires a clean click at the look point per
    // pending press (counts, not a single flag, so a fast second click before the tick drains isn't lost).
    std::atomic<unsigned> moveSignals{0};    // hook -> tick: WM_MOUSEMOVE seen (repan wake, #195)
    std::atomic<bool> inspectActive{false};  // tick -> hook: Inspect on, swallow clicks
    std::atomic<int>  commitLeft{0};         // hook -> tick: pending left clicks to fire at the look point
    std::atomic<int>  commitRight{0};        // hook -> tick: pending right clicks
    // --- Diagnostics for the intermittent stuck-side-button (issue #113). Each side-button
    // observation site bumps the matching counter (atomic fetch_add only - NO I/O, safe in the LL
    // hook); the tick thread reads these and logs a snapshot on every held-state edge, so a stuck
    // (a rising edge with no matching fall) is captured along with whether the hook and/or Raw Input
    // ever saw the release. Index by xbutton id (1 = XBUTTON1, 2 = XBUTTON2); [0] unused.
    std::atomic<unsigned> dbgHookDown[3]{};  // WM_XBUTTONDOWN seen by the LL mouse hook
    std::atomic<unsigned> dbgHookUp[3]{};    // WM_XBUTTONUP   seen by the LL mouse hook
    std::atomic<unsigned> dbgHookDbl[3]{};   // WM_XBUTTONDBLCLK seen by the hook (currently ignored by state)
    std::atomic<unsigned> dbgRawDown[3]{};   // RI_MOUSE_BUTTON_4/5_DOWN seen in WM_INPUT
    std::atomic<unsigned> dbgRawUp[3]{};     // RI_MOUSE_BUTTON_4/5_UP   seen in WM_INPUT
};

class InputRouter {
public:
    // Each direction can have a primary AND an alternate side-button (1 = XBUTTON1, 2 = XBUTTON2,
    // 0 = none); the two are OR-combined so either press zooms that direction. swallow: block the
    // bound buttons from reaching other apps while running.
    bool start(int inButtonId, int inButtonId2, int outButtonId, int outButtonId2, bool swallow);
    void stop();
    InputState& state() { return state_; }
    // Auto-reset event pulsed by the LL mouse hook on every REAL (non-injected) pointer move
    // (issue #195). The transform model's pan write must be coherent with the cursor sample
    // that caused it - native writes from inside its hook; the Magnification API is
    // thread-affine, so the tick thread waits on this instead and writes the moment the
    // pointer really moved. Never signalled for injected moves (our own weld/click warps).
    // Valid between start() and stop(); null when the hook is not installed (WIND_NOHOOK).
    // void*, not HANDLE: this header stays free of <windows.h> (pure-logic build rule).
    void* mouseMoveEvent() const { return mouseMoveEvent_; }
    // Atomically read and zero the accumulated raw deltas.
    void drainRaw(int& dx, int& dy);
    // Inspect-mode speed match: the OS cursor is frozen, so the look point pans from raw mickeys.
    // Raw mickeys are pre-acceleration / pre-pointer-speed, so they feel slower than the desktop
    // cursor. setBallistics() supplies the current Windows pointer-speed + acceleration settings;
    // cookPacket() (called per WM_INPUT packet) converts that packet to cooked pixels and sums them
    // while Inspect is active; drainCooked() reads and zeroes the accumulated cooked delta. All of
    // this runs on the main thread (WM_INPUT, the tick, and the setters are all main-thread).
    void setBallistics(const BallisticsConfig& c) { ballistics_ = c; }
    void cookPacket(int dx, int dy);
    void drainCooked(double& dx, double& dy) { dx = cookedX_; dy = cookedY_; cookedX_ = 0.0; cookedY_ = 0.0; }
    // Map an XBUTTON id (1 = XBUTTON1, 2 = XBUTTON2) to the in/out held state, using the
    // configured zoom buttons. Shared by the WH_MOUSE_LL hook and main's WM_INPUT path.
    void setButtonState(int xbuttonId, bool down);
    // Whether the id is one of the configured zoom buttons (used to decide swallowing).
    bool isZoomButton(int xbuttonId) const;
    // Whether the hook should swallow the configured zoom buttons (set in start()).
    bool swallowEnabled() const { return swallow_; }
    // --- Keyboard binds (WH_KEYBOARD_LL hook) -------------------------------------------------
    // Configure the keyboard VKs the keyboard hook tracks + swallows (zoom in/out primary+alt,
    // recenter, and the Inspect-mode cursor-lock toggle; 0 = unbound).
    // Forbidden VKs (IsForbiddenBindVk) are stored but never acted on.
    // Clears the per-key pressed/swallowed records so a remap mid-press can't strand a key.
    void setKeys(int zoomInVk, int zoomInVk2, int zoomOutVk, int zoomOutVk2, int recenterVk,
                 int cursorLockVk);
    // Whether vk is one of the configured (non-forbidden) keyboard binds: decides track+swallow.
    bool isBoundKey(int vk) const;
    // Physical down-state of a keyboard key, as tracked by the keyboard hook. This is the authority
    // when kbHookActive() (a swallowed key never shows in GetAsyncKeyState), so main reads it instead
    // of polling. Returns false for out-of-range vk.
    bool keyPressed(int vk) const;
    // Raw Input safety net (issue #167): clear a key's held/swallowed records from a WM_INPUT key
    // UP. Raw Input is NOT subject to LowLevelHooksTimeout, so it still arrives when Windows has
    // silently evicted the hook mid-hold - the case that otherwise strands a keyboard zoom bind as
    // held forever. UP only: it can only ever clear held state, never set it, so it cannot falsely
    // hold a key and is idempotent with the hook's own clear. The mouse side-buttons have had the
    // same net since #113; this is the keyboard half.
    void rawKeyUp(int vk);
    // Same safety net for the side-buttons: honor a WM_INPUT button UP unless the live hook saw a
    // DOWN for that button within the last ~30 ms (a queued raw UP processed after the hook's next
    // DOWN would cancel a hold that is physically down - no auto-repeat re-asserts a button).
    void rawButtonUp(int xbuttonId);
    // Hook-thread recency stamps consumed by the two guards above.
    void noteHookKeyDown(int vk);
    void noteHookButtonDown(int xbuttonId);
    // True once the LL KEYBOARD hook is installed. When false (install failed or WIND_NOHOOK), main
    // must fall back to GetAsyncKeyState and no keyboard swallowing happens.
    bool kbHookActive() const { return kbHookActive_.load(std::memory_order_relaxed); }
    // --- LL keyboard hook watchdog (issue #156) ------------------------------------------------
    // Windows SILENTLY evicts a low-level hook whose callback misses LowLevelHooksTimeout. There is
    // no notification and no error: the hook handle stays non-null, KbProc simply never fires again.
    // A game's launch load spike is exactly when that happens, which is why "start Wind, then launch
    // the game" left every keyboard bind dead while the mouse binds survived - and why rebinding a
    // key then looked like a broken ini hot-reload (the reload DID apply; the dead hook just never
    // reported the key, and kbHookActive() still claimed the hook was the authority).
    //
    // Called from the tick thread when it detects the eviction (see the watchdog in main.cpp).
    // Immediately clears kbHookActive_ so main falls back to GetAsyncKeyState polling on the very
    // next tick - a dead hook swallows nothing, so polling is correct and the binds work again at
    // once - then asks the hook thread to re-install (a hook must be installed by the thread that
    // pumps it, so this posts rather than calling SetWindowsHookEx here).
    void requestKbHookReinstall();
    // Hook thread -> publish the new installed state after an install/uninstall.
    void onKbHookStateChanged(bool active);
    // --- Keyboard-hook suspension in games -----------------------------------------------------
    // A WH_KEYBOARD_LL hook taxes the SYSTEM's input pipeline, not just ours: the raw input thread
    // dispatches every keystroke to the hooking thread and waits for it to return before delivering
    // any further input - including mouse movement to the foreground game. Holding a key in a game
    // (auto-repeat, ~30/s) therefore punches a stall into the mouse stream on every repeat: the
    // "panning is smooth until I hold a key" stutter. The cost is the hook's EXISTENCE, not our
    // callback (atomics-only) and not swallowing - an unbound key like Ctrl stalls identically, and
    // the stutter vanished entirely whenever Windows had evicted the hook.
    //
    // Swallowing a keyboard bind is worthless in a raw-input game anyway (documented limitation: an
    // LL hook cannot block raw input, which is what games read), so the hook buys nothing there
    // while costing the game its pacing. Idempotent: only posts to the hook thread on a change.
    void setKeyboardHookWanted(bool want);
    // Count of successful re-installs this session (diagnostics / tests).
    unsigned kbHookReinstalls() const { return kbHookReinstalls_.load(std::memory_order_relaxed); }
    // Magnify model only: make the keyboard hook skip INJECTED events entirely. The magnify model
    // drives Windows Magnifier by injecting Win+Plus/Win+Minus chords, and NumPad +/- are bindable
    // zoom keys - without the skip, our own injection would be swallowed by our own hook and
    // re-registered as a zoom press (a feedback loop). Off by default so tools that inject keys
    // (e.g. AutoHotkey remaps) keep working with the render model.
    void setIgnoreInjectedKeys(bool on) { ignoreInjectedKeys_.store(on, std::memory_order_relaxed); }
    bool ignoreInjectedKeys() const { return ignoreInjectedKeys_.load(std::memory_order_relaxed); }
    // True when the LL mouse hook is installed (the normal build). When true the hook is the SOLE
    // authority for side-button held state; main's WM_INPUT path must NOT also write button state
    // (Raw Input still delivers the transition even though the hook swallows the legacy message, so
    // both writing would race/double-count). WM_INPUT button writes are only the WIND_NOHOOK fallback.
    bool hookActive() const { return hookActive_.load(std::memory_order_relaxed); }
    // Live-rebind the configured zoom buttons (called from the tick thread on hot-reload).
    // Atomic so the hook thread's reads in setButtonState/isZoomButton stay race-free, and the
    // held flags are cleared so a stale press of the previous button does not stick.
    void setButtons(int inButtonId, int inButtonId2, int outButtonId, int outButtonId2);
private:
    InputState state_;
    // Primary + alternate side-button per direction (1 = XBUTTON1, 2 = XBUTTON2, 0 = none); set in
    // start(). A direction is "held" if either of its bound buttons is down (OR-combined).
    std::atomic<int> inButtonId_{2};
    std::atomic<int> inButtonId2_{0};
    std::atomic<int> outButtonId_{1};
    std::atomic<int> outButtonId2_{0};
    bool swallow_ = true;
    void*  mouseMoveEvent_ = nullptr;       // pulsed by the mouse hook on real pointer motion (#195)
    std::atomic<bool> hookActive_{false};   // true once the LL hook is installed (not WIND_NOHOOK)
    // Configured keyboard binds (VK codes; 0 = unbound). Atomic so the keyboard hook thread reads
    // them race-free against setKeys() on the tick thread. hideCursor/quickZoom binds are NOT here:
    // they use RegisterHotKey, which already suppresses the key from other apps.
    std::atomic<int> kbZoomInVk_{0};
    std::atomic<int> kbZoomInVk2_{0};
    std::atomic<int> kbZoomOutVk_{0};
    std::atomic<int> kbZoomOutVk2_{0};
    std::atomic<int> kbRecenterVk_{0};
    std::atomic<int> kbCursorLockVk_{0};
    // Recency stamps for the raw-UP reordering guards (see rawKeyUp/rawButtonUp).
    std::atomic<unsigned long long> kbLastHookDownMs_[256]{};
    std::atomic<unsigned long long> btnLastHookDownMs_[3]{};
    std::atomic<bool> kbHookActive_{false}; // true once the LL KEYBOARD hook is installed
    std::atomic<unsigned> kbHookReinstalls_{0};  // watchdog recoveries this session
    std::atomic<bool> kbHookWanted_{true};       // false while a fullscreen game is foreground
    std::atomic<bool> kbHookRecovering_{false};  // distinguishes watchdog recovery from a resume
    std::atomic<bool> ignoreInjectedKeys_{false}; // magnify model: kb hook skips LLKHF_INJECTED
    // Inspect-mode cooked-pixel accumulator (main-thread only: WM_INPUT cooks, the tick drains).
    BallisticsConfig ballistics_{};
    double cookedX_ = 0.0;
    double cookedY_ = 0.0;
};

// Called from main.cpp's WM_INPUT handler with decoded relative mouse deltas.
void AccumulateRaw(InputRouter& r, int dx, int dy);
}
