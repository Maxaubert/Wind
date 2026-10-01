#pragma once
// Who owns the zoomed view (issue #276). Pure: no <windows.h>, tests/test_view_target.cpp.
//
// Most recent input wins, with three owner rules from the first field test (2026-09-29):
//  - The mouse takes the view back only on REAL movement (3 px within 100 ms) or a button, so
//    sensor jitter while typing never steals it.
//  - When the mouse MOVES while the view shows the caret/focus, the POINTER comes to the view
//    (warpPointer: the caller places it at the view centre), the view stays. A button press
//    instead gives the view back to the pointer without moving it: warping under a held button
//    would drag. (The old "glide the view back" chased a moving pointer and never arrived - the
//    field-reported wobble.)
//  - Caret/focus changes within kClickQuietMs of a mouse button are the click's own doing
//    (opening a page, clicking into a field): they are consumed without taking the view.
//  - And they need a KEY: a caret/focus change takes the view only if a key went down or up within
//    kKeyDrivenMs (issue #289; key-ups count so Alt+Tab released after a long look still counts). Scrolling a page moves a focused control's caret on screen with
//    no key at all (field: the Settings page dragged the view while scrolling).
#include <cmath>
namespace wind {
enum class ViewOwner { Mouse, Caret, Focus, Keys };   // Keys: keyboard panning (#287)
enum class TrackKind { None = 0, Caret = 1, Focus = 2 };
struct TrackSnapshot { TrackKind kind = TrackKind::None; unsigned seq = 0; double l = 0, t = 0, r = 0, b = 0; };
struct ViewOwnerState {
    ViewOwner owner = ViewOwner::Mouse;
    unsigned lastSeq = 0;
    double moveAccum = 0;
    double moveWindowMs = 0;
    bool warpPointer = false;     // set on the tick the mouse MOVED the view back; caller clears
    // The caret/focus rect the view follows: LATCHED only from events that passed the gates. Reading
    // the live snapshot instead let an owner that already followed one keystroke keep chasing every
    // later caret move, a scroll included, and an app-driven focus change (review 2026-09-30).
    TrackSnapshot target;
    // Zoom-in settle (#310): enabling the tracker makes it publish the CURRENT caret, which within
    // a second of any key press read as a new keyboard-driven event: the view lurched to the caret
    // and the pointer was warped back (~3% of zoom-ins). Events inside this window only re-baseline.
    // StepViewOwner runs only while zoomed, so the caller clears wasEnabled on every 1x tick
    // (main.cpp); a state that never saw a disabled tick counts as already settled.
    bool wasEnabled = true;
    double settleMs = 0;
};
struct ViewOwnerInputs {
    bool enabled = false;         // zoomed && !game && !inspect && !locked
    bool trackCaret = false, trackFocus = false;
    double mouseDx = 0, mouseDy = 0;   // real pointer movement this tick, px
    bool buttonDown = false;
    double msSinceButton = 1e9;   // time since a mouse button was last down
    double msSinceKey = 0;        // time since any key went down (0 when unknown: no gate)
    double dtMs = 0;
    TrackSnapshot snap;
    bool panning = false;         // a pan key is held or its motion is still gliding (#287)
};
inline constexpr double kMouseTakeoverPx = 3.0, kMouseTakeoverWindowMs = 100.0;
inline constexpr double kClickQuietMs = 1000.0;
inline constexpr double kKeyDrivenMs = 1000.0;
inline constexpr double kEnableSettleMs = 150.0;

inline ViewOwner StepViewOwner(ViewOwnerState& s, const ViewOwnerInputs& in) {
    s.warpPointer = false;
    const bool detached = s.owner != ViewOwner::Mouse;
    if (in.enabled && !s.wasEnabled) s.settleMs = kEnableSettleMs;   // just enabled (zoom-in)
    s.wasEnabled = in.enabled;
    if (!in.enabled) {
        s.owner = ViewOwner::Mouse;   // zoomed out / game / Inspect: the mouse path resumes
        s.lastSeq = in.snap.seq;      // events seen while disabled never fire later
        s.moveAccum = 0; s.moveWindowMs = 0;
        return s.owner;
    }
    // Mouse activity: accumulate movement inside a sliding window.
    const double step = std::fabs(in.mouseDx) + std::fabs(in.mouseDy);
    if (step > 0) {
        if (s.moveWindowMs > kMouseTakeoverWindowMs) { s.moveAccum = 0; s.moveWindowMs = 0; }
        s.moveAccum += step;
    }
    s.moveWindowMs += in.dtMs;
    if (s.moveWindowMs > kMouseTakeoverWindowMs && step == 0) { s.moveAccum = 0; s.moveWindowMs = 0; }
    const bool moved = s.moveAccum >= kMouseTakeoverPx;
    if (in.buttonDown || moved) {
        s.moveAccum = 0; s.moveWindowMs = 0;
        if (detached) {
            s.owner = ViewOwner::Mouse;
            s.warpPointer = moved && !in.buttonDown;
        }
        s.lastSeq = in.snap.seq;      // the mouse wins this tick
        return s.owner;
    }
    // Keyboard panning is an explicit request: it owns the view while active, ahead of any caret or
    // focus event in the same tick. The mouse still takes it back exactly as from the caret (#287).
    if (in.panning) {
        s.owner = ViewOwner::Keys;
        s.lastSeq = in.snap.seq;
        return s.owner;
    }
    // Settle window after enabling: the tracker's activation publish is a baseline, not typing.
    if (s.settleMs > 0) {
        s.settleMs -= in.dtMs;
        s.lastSeq = in.snap.seq;
        return s.owner;
    }
    // A new tracking event, unless a recent click caused it.
    if (in.snap.seq != s.lastSeq) {
        s.lastSeq = in.snap.seq;
        if (in.msSinceButton >= kClickQuietMs && in.msSinceKey <= kKeyDrivenMs) {
            if (in.snap.kind == TrackKind::Caret && in.trackCaret) { s.owner = ViewOwner::Caret; s.target = in.snap; }
            else if (in.snap.kind == TrackKind::Focus && in.trackFocus) { s.owner = ViewOwner::Focus; s.target = in.snap; }
        }
    }
    return s.owner;
}
}  // namespace wind
