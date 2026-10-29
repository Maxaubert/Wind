import Foundation

// Turns key and mouse button events into held actions and taps, and decides which events are
// swallowed. Pure: the event tap feeds it and acts on the result.
//
// Rules carried over from Wind:
//  - A bind fires on the trigger going down with exactly its modifiers held.
//  - A held action ends when its trigger goes up, whatever the modifiers do meanwhile.
//  - Swallowing is balanced: a trigger whose down was swallowed has its up (and auto-repeats)
//    swallowed too, and nothing else.

public struct InputRouter {
    public private(set) var binds: [Action: [Bind]] = [:]
    /// Triggers currently held down that we swallowed, and the action each one started.
    public private(set) var swallowed: [Trigger: Action] = [:]

    public init() {}

    public mutating func setBinds(_ b: [Action: [Bind]]) {
        binds = b
    }

    public func isHeld(_ a: Action) -> Bool { swallowed.values.contains(a) }

    public enum Edge: Equatable { case down(Action), up(Action) }

    /// Returns (swallow, edge). Repeats of a swallowed key are swallowed with no edge.
    public mutating func triggerDown(_ t: Trigger, mods: Mods, isRepeat: Bool = false) -> (Bool, Edge?) {
        if swallowed[t] != nil { return (true, nil) }
        if isRepeat { return (false, nil) }
        for a in Action.allCases {
            for b in binds[a] ?? [] where b.trigger == t && modsMatch(mods, b.mods) {
                swallowed[t] = a
                return (true, .down(a))
            }
        }
        return (false, nil)
    }

    public mutating func triggerUp(_ t: Trigger) -> (Bool, Edge?) {
        if let a = swallowed.removeValue(forKey: t) { return (true, .up(a)) }
        return (false, nil)
    }

    /// Forget everything held (tap re-enabled after a timeout, or teardown).
    public mutating func releaseAll() -> [Action] {
        let held = Array(swallowed.values)
        swallowed.removeAll()
        return held
    }
}
