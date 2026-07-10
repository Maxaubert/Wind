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
    // recenter, and the Inspect-mode cursor-lock toggle; 0 = unbound). Forbidden VKs
    // (IsForbiddenBindVk) are stored but never acted on.
    // Clears the per-key pressed/swallowed records so a remap mid-press can't strand a key.
    void setKeys(int zoomInVk, int zoomInVk2, int zoomOutVk, int zoomOutVk2, int recenterVk,
                 int cursorLockVk, int swapModelVk);
    // Whether vk is one of the configured (non-forbidden) keyboard binds: decides track+swallow.
    bool isBoundKey(int vk) const;
    // Physical down-state of a keyboard key, as tracked by the keyboard hook. This is the authority
    // when kbHookActive() (a swallowed key never shows in GetAsyncKeyState), so main reads it instead
    // of polling. Returns false for out-of-range vk.
    bool keyPressed(int vk) const;
    // True once the LL KEYBOARD hook is installed. When false (install failed or WIND_NOHOOK), main
    // must fall back to GetAsyncKeyState and no keyboard swallowing happens.
    bool kbHookActive() const { return kbHookActive_.load(std::memory_order_relaxed); }
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
    std::atomic<int> kbSwapModelVk_{0};
    std::atomic<bool> kbHookActive_{false}; // true once the LL KEYBOARD hook is installed
    // Inspect-mode cooked-pixel accumulator (main-thread only: WM_INPUT cooks, the tick drains).
    BallisticsConfig ballistics_{};
    double cookedX_ = 0.0;
    double cookedY_ = 0.0;
};

// Called from main.cpp's WM_INPUT handler with decoded relative mouse deltas.
void AccumulateRaw(InputRouter& r, int dx, int dy);
}
