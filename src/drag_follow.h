#pragma once
// Pure decision: should the render model stop welding the OS cursor and FOLLOW it instead,
// because the user is mid-drag? (issue #169)
//
// While a mouse button is held, the OS pointer IS the interaction - a window drag or a text
// selection consumes its position directly. The render model's per-tick weld (SetCursorPos to the
// smoothed lens centre) then fights the hand: the pointer alternates between where the hand pushed
// it and where the weld parks it, and everything following the pointer flickers between the two.
// Probe-measured on the rig: ~85 px square-wave oscillation, amplitude scaling with hand speed.
//
// So for exactly the duration of a button-hold in a FREE render session, the weld is suspended and
// the lens follows the pointer 1:1 (the transform model's FOLLOW design, proven on the desktop).
// Click alignment is unaffected: the press landed under the welded cursor (the weld was live until
// the button went down), and the release lands where the pointer and the dragged content both are.
//
// The other regimes keep their own cursor policy and never drag-follow:
//   locked     - a game owns the pointer (clip/recenter); panning is raw-mickey driven.
//   gameFreeze - transform game session, pointer frozen by design.
//   inspect    - pointer frozen at the anchor; the look point pans instead.
// Pure logic (no windows.h) so the truth table is unit-testable.
namespace wind {

inline bool ShouldDragFollow(bool renderActive, bool locked, bool gameFreeze, bool inspect,
                             bool anyButtonDown) {
    return renderActive && anyButtonDown && !locked && !gameFreeze && !inspect;
}

}  // namespace wind
