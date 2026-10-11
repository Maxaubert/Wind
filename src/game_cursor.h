#pragma once
// Game-drawn cursor (issue #443) - PURE, no <windows.h>, tests/test_game_cursor.cpp.
//
// Some games hide the Windows pointer and draw their own cursor at the pointer's position (Cyberpunk
// 2077 menus). Their cursor reaches the screen a display frame or so after the pointer moved, while
// DWM centring puts the view on the newest pointer position, so the game's cursor swung behind every
// movement and flipped side on each change of direction: rms 54 screen px at 6.5x and 1000 desktop
// px/s (measured 2026-10-11). While a covering foreground app hides the pointer, Wind writes the view
// itself and follows the pointer gameCursorLagMs late (default one display frame): rms 11-35 px.
// Following without the delay (Wind's write, no DWM centring) measured the same as DWM centring, so
// the gain is the delay. The rest is the game's own frame timing.
namespace wind {

// A game draws its own cursor: a free-pointer transform session over a foreground that covers the
// monitor, with the pointer hidden by that app (not by Wind's hide-cursor hotkey or setting).
inline bool GameDrawsCursor(bool freeCursor, bool fsCover, bool pointerShowing, bool hiddenByWind) {
    return freeCursor && fsCover && !pointerShowing && !hiddenByWind;
}

// On only while the hidden pointer MOVES (review of #444): a fullscreen video player hiding an idle
// pointer, or Windows hiding it while typing, also clears CURSOR_SHOWING over a covering window, and
// the hand-over to Wind's own write moves the view by DWM's pixel (#445). A pointer that is hidden and
// still never switches; a game menu hides it while the hand moves it, which also hides the hand-over.
// Off as soon as the pointer shows or the window stops qualifying.
inline bool GameCursorStep(bool wasOn, bool drawsCursor, bool handMoved) {
    return drawsCursor && (wasOn || handMoved);
}

// The delay in ms: cfg < 0 means one display frame at hz, 0 turns the delay off (DWM centring as for
// any other window), anything else is used as given (capped at 100 ms).
inline double GameCursorLagMs(int cfgMs, double hz) {
    if (cfgMs == 0) return 0.0;
    if (cfgMs > 0) return cfgMs > 100 ? 100.0 : double(cfgMs);
    return hz > 0.0 ? 1000.0 / hz : 1000.0 / 60.0;
}

// The pointer's recent positions, one sample per tick, read back at an earlier time.
struct PointerHistory {
    static constexpr int kN = 64;   // 440 ms at 144 Hz, far more than any delay
    struct S { double ms, x, y; };
    S   s[kN] = {};
    int head = 0, count = 0;

    void push(double ms, double x, double y) {
        s[head] = { ms, x, y };
        head = (head + 1) % kN;
        if (count < kN) ++count;
    }
    void clear() { head = 0; count = 0; }
    // The position at time ms, interpolated between the two samples around it. Before the oldest
    // sample it is the oldest one; with no samples it reports false.
    bool at(double ms, double& x, double& y) const {
        if (count == 0) return false;
        const S* newer = &s[(head - 1 + kN) % kN];
        if (newer->ms <= ms) { x = newer->x; y = newer->y; return true; }
        for (int k = 2; k <= count; ++k) {
            const S* older = &s[(head - k + kN) % kN];
            if (older->ms <= ms) {
                const double span = newer->ms - older->ms;
                const double f = span > 0.0 ? (ms - older->ms) / span : 1.0;
                x = older->x + (newer->x - older->x) * f;
                y = older->y + (newer->y - older->y) * f;
                return true;
            }
            newer = older;
        }
        x = newer->x; y = newer->y;
        return true;
    }
};

}  // namespace wind
