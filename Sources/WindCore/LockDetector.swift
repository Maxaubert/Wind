import Foundation

// Detects a game that has locked the pointer (CGAssociateMouseAndMouseCursorPosition(false), as
// macOS games do for mouse-look and for their own drawn cursor): raw deltas keep arriving but the
// pointer does not move. While locked, the view pans from the deltas instead of the pointer.
//
// A pointer pushed into a screen edge also stops while deltas flow; the caller passes `pinned` so
// that never counts as a lock (Wind's PointerPinnedAtEdge lesson).

public struct LockDetector {
    public private(set) var locked = false
    var windowMs = 0.0
    var deltaSum = 0.0
    var travelSum = 0.0
    var lastPos: Pt?
    var forced = false

    /// Sliding window over which deltas and real travel are compared.
    public static let windowLength = 120.0
    /// Enough hand motion inside one window to judge.
    public static let minDelta = 24.0

    public init() {}

    public mutating func force(_ on: Bool) { forced = on; if on { locked = true } }

    public mutating func reset() {
        locked = forced; windowMs = 0; deltaSum = 0; travelSum = 0; lastPos = nil
    }

    /// Feed one mouse-move event. dtMs is the time since the previous event. level is the zoom:
    /// while zoomed macOS moves the pointer only 1/level as far, so travel is judged at 1x scale.
    public mutating func feed(dx: Double, dy: Double, pos: Pt, dtMs: Double, pinned: Bool, level: Double = 1) {
        let travel: Double
        if let lp = lastPos { travel = (abs(pos.x - lp.x) + abs(pos.y - lp.y)) * max(1, level) } else { travel = 0 }
        lastPos = pos
        if forced { locked = true; return }
        if pinned && !locked { windowMs = 0; deltaSum = 0; travelSum = 0; return }
        windowMs += max(0, dtMs)
        deltaSum += abs(dx) + abs(dy)
        travelSum += travel
        if deltaSum >= LockDetector.minDelta || windowMs >= LockDetector.windowLength {
            if deltaSum >= LockDetector.minDelta {
                if !locked && travelSum < 1.5 { locked = true }
                else if locked && travelSum >= deltaSum * 0.3 { locked = false }
            }
            windowMs = 0; deltaSum = 0; travelSum = 0
        }
    }
}
