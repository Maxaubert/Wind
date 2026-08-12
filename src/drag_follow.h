#pragma once
// Pure decision: should the active model stop welding the OS cursor and FOLLOW it instead,
// because the user is mid-drag? (issue #169)
//
// While a mouse button is held, the OS pointer IS the interaction - a window drag or a text
// selection consumes its position directly. The per-tick weld (SetCursorPos to the smoothed lens
// centre) then fights the hand: the pointer alternates between where the hand pushed it and where
// the weld parks it, and everything following the pointer flickers between the two.
// Probe-measured on the rig: ~85 px square-wave oscillation, amplitude scaling with hand speed.
//
// So for exactly the duration of a button-hold in a FREE welded session, the weld is suspended and
// the lens follows the pointer 1:1. Both welding models (render AND transform - the transform
// welds too since the 8a52040 re-test) take this path; scaling would desync the lens from the
// pointer that owns the drag. Click alignment is unaffected: the press landed under the welded
// cursor (the weld was live until the button went down), and the release lands where the pointer
// and the dragged content both are.
//
// The other regimes keep their own cursor policy and never drag-follow:
//   locked  - a game owns the pointer (clip/recenter); panning is raw-mickey driven.
//   inspect - pointer frozen at the anchor; the look point pans instead.
// Pure logic (no windows.h) so the truth table is unit-testable.
namespace wind {

inline bool ShouldDragFollow(bool weldActive, bool locked, bool inspect, bool anyButtonDown) {
    return weldActive && anyButtonDown && !locked && !inspect;
}

}  // namespace wind
