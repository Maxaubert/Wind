#pragma once
// Who owns the zoomed view (issue #276). Pure: no <windows.h>, tests/test_view_target.cpp.
// Most recent input wins; the mouse takes the view back only on REAL movement (3 px within 100 ms)
// or a button, so sensor jitter while typing never steals it. Leaving Caret/Focus always goes via
// Returning, a glide back to the pointer that the caller ends with FinishReturn().
#include <cmath>
namespace wind {
enum class ViewOwner { Mouse, Caret, Focus, Returning };
enum class TrackKind { None = 0, Caret = 1, Focus = 2 };
struct TrackSnapshot { TrackKind kind = TrackKind::None; unsigned seq = 0; double l = 0, t = 0, r = 0, b = 0; };
struct ViewOwnerState { ViewOwner owner = ViewOwner::Mouse; unsigned lastSeq = 0; double moveAccum = 0; double moveWindowMs = 0; };
struct ViewOwnerInputs {
    bool enabled = false;
    bool trackCaret = false, trackFocus = false;
    double mouseDx = 0, mouseDy = 0;
    bool buttonDown = false;
    double dtMs = 0;
    TrackSnapshot snap;
};
inline constexpr double kMouseTakeoverPx = 3.0, kMouseTakeoverWindowMs = 100.0;

inline void FinishReturn(ViewOwnerState& s) { s.owner = ViewOwner::Mouse; }

inline ViewOwner StepViewOwner(ViewOwnerState& s, const ViewOwnerInputs& in) {
    const bool detached = s.owner == ViewOwner::Caret || s.owner == ViewOwner::Focus;
    if (!in.enabled) {
        if (detached) s.owner = ViewOwner::Returning;
        s.lastSeq = in.snap.seq;     // events seen while disabled never fire later
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
    const bool mouseActive = in.buttonDown || s.moveAccum >= kMouseTakeoverPx;
    if (mouseActive) {
        s.moveAccum = 0; s.moveWindowMs = 0;
        if (detached) s.owner = ViewOwner::Returning;
        s.lastSeq = in.snap.seq;     // the mouse wins this tick
        return s.owner;
    }
    // A new tracking event.
    if (in.snap.seq != s.lastSeq) {
        s.lastSeq = in.snap.seq;
        if (in.snap.kind == TrackKind::Caret && in.trackCaret) s.owner = ViewOwner::Caret;
        else if (in.snap.kind == TrackKind::Focus && in.trackFocus) s.owner = ViewOwner::Focus;
    }
    return s.owner;
}
}  // namespace wind
