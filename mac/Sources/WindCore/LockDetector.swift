import Foundation

// Detects a game that has taken the pointer for mouse-look or for its own drawn cursor. macOS
// games hide the system cursor AND disassociate it from the mouse, so the signal is: the cursor is
// hidden and the pointer stays put while the hand keeps moving.
//
// Hidden alone is not enough: after typing, macOS can keep reporting the cursor hidden for many
// seconds while it visibly moves (recorded 2026-10-30 in Claude Code: a false lock held the view
// for 14 s). There the pointer follows the hand, so `pointerMoved` keeps the lock off, and a held
// lock is released as soon as the pointer moves with the hand again.
//
// Pointer travel is NOT used: while zoomed, Wind's own view writes move the pointer's desktop
// position, which read as hand movement and released every lock at once (measured 2026-10-29).

public struct LockDetector {
    public private(set) var locked = false
    var hiddenMotion = 0.0
    var forced = false
    var followTicks = 0

    /// Hand motion (raw delta, points) with the cursor hidden before it counts as a game.
    public static let minHiddenMotion = 24.0

    public init() {}

    public mutating func force(_ on: Bool) { forced = on; if on { locked = true } }

    public mutating func reset() {
        locked = forced; hiddenMotion = 0
    }

    /// Feed one tick: whether the system cursor is visible, the hand motion since the last tick,
    /// and how far the pointer itself moved (SCREEN points, Wind's own writes excluded).
    public mutating func feed(cursorVisible: Bool, motion: Double, pointerMoved: Double = 0) {
        if forced { locked = true; return }
        // Following the hand must hold for two ticks in a row: a single tick can still carry an
        // echo of Wind's own view writes.
        followTicks = (motion > 0 && pointerMoved > 0.5) ? followTicks + 1 : 0
        if cursorVisible || followTicks >= 2 {
            locked = false; hiddenMotion = 0
            return
        }
        hiddenMotion += motion
        if hiddenMotion >= LockDetector.minHiddenMotion { locked = true }
    }
}
