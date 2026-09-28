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
#include <cmath>
namespace wind {
enum class ViewOwner { Mouse, Caret, Focus };
enum class TrackKind { None = 0, Caret = 1, Focus = 2 };
struct TrackSnapshot { TrackKind kind = TrackKind::None; unsigned seq = 0; double l = 0, t = 0, r = 0, b = 0; };
struct ViewOwnerState {
    ViewOwner owner = ViewOwner::Mouse;
    unsigned lastSeq = 0;
    double moveAccum = 0;
    double moveWindowMs = 0;
    bool warpPointer = false;     // set on the tick the mouse MOVED the view back; caller clears
};
struct ViewOwnerInputs {
    bool enabled = false;         // zoomed && !game && !inspect && !locked
    bool trackCaret = false, trackFocus = false;
    double mouseDx = 0, mouseDy = 0;   // real pointer movement this tick, px
    bool buttonDown = false;
    double msSinceButton = 1e9;   // time since a mouse button was last down
    double dtMs = 0;
    TrackSnapshot snap;
};
inline constexpr double kMouseTakeoverPx = 3.0, kMouseTakeoverWindowMs = 100.0;
inline constexpr double kClickQuietMs = 1000.0;

inline ViewOwner StepViewOwner(ViewOwnerState& s, const ViewOwnerInputs& in) {
    s.warpPointer = false;
    const bool detached = s.owner != ViewOwner::Mouse;
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
    // A new tracking event, unless a recent click caused it.
    if (in.snap.seq != s.lastSeq) {
        s.lastSeq = in.snap.seq;
        if (in.msSinceButton >= kClickQuietMs) {
            if (in.snap.kind == TrackKind::Caret && in.trackCaret) s.owner = ViewOwner::Caret;
            else if (in.snap.kind == TrackKind::Focus && in.trackFocus) s.owner = ViewOwner::Focus;
        }
    }
    return s.owner;
}
}  // namespace wind
