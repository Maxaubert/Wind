#pragma once
// Pure hybrid engine-pick decision (no <windows.h>), extracted so the most regression-prone
// predicate in the app is unit-tested (issues #148 exclusion, #172 shell desktop) instead of
// living inline in two RunTick sites that had to stay identical by hand.
//
// transform is picked ONLY for a borderless foreground that covers the PRIMARY target monitor
// (games, F11 video): compositor-internal magnification survives a heavy game's present load.
// Everything else gets the render engine:
//  - a maximized desktop app covers but keeps its caption -> render (documented trap),
//  - the shell desktop (Win+D) reads as a borderless cover -> render (issue #172),
//  - excluded exes (fullscreen browser video wants a desktop-style cursor) -> render,
//  - learned cursor-shape churners -> render, unless the tdrTest harness forces transform,
//  - any non-primary monitor -> render (no cross-adapter transform chase).
namespace wind {

struct EnginePickInputs {
    bool coversMonitor  = false;  // foreground covers the session's target monitor
    bool borderless     = false;  // foreground has no WS_CAPTION
    bool primaryMonitor = false;  // the target monitor is the primary
    bool shellDesktop   = false;  // foreground is the shell desktop window class (issue #172)
    bool excluded       = false;  // exe listed in transformExclude (issue #148)
    bool churny         = false;  // exe learned in churny_apps.txt
    bool tdrHarness     = false;  // cfg.tdrTest > 0: bypass the churny list for field experiments
};

inline bool ShouldPickTransform(const EnginePickInputs& in) {
    return in.coversMonitor && in.borderless && in.primaryMonitor &&
           !in.shellDesktop && !in.excluded && (in.tdrHarness || !in.churny);
}

}  // namespace wind
