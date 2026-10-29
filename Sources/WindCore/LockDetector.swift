import Foundation

// Detects a game that has taken the pointer for mouse-look or for its own drawn cursor. macOS
// games hide the system cursor and disassociate it from the mouse, so the signal is: the cursor
// stays hidden while the mouse keeps moving. (Typing hides the cursor too, but macOS shows it on
// the first movement, so a sustained hidden-while-moving state means a game.)
//
// Pointer travel is NOT used: while zoomed, Wind's own view writes move the pointer's desktop
// position, which read as hand movement and released every lock at once (measured 2026-10-29).

public struct LockDetector {
    public private(set) var locked = false
    var hiddenMotion = 0.0
    var forced = false

    /// Hand motion (raw delta, points) with the cursor hidden before it counts as a game.
    public static let minHiddenMotion = 24.0

    public init() {}

    public mutating func force(_ on: Bool) { forced = on; if on { locked = true } }

    public mutating func reset() {
        locked = forced; hiddenMotion = 0
    }

    /// Feed one tick: whether the system cursor is visible and the hand motion since the last tick.
    public mutating func feed(cursorVisible: Bool, motion: Double) {
        if forced { locked = true; return }
        if cursorVisible {
            locked = false; hiddenMotion = 0
            return
        }
        hiddenMotion += motion
        if hiddenMotion >= LockDetector.minHiddenMotion { locked = true }
    }
}
