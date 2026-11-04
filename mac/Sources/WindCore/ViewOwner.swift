// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
import Foundation

// Who owns the zoomed view (port of Wind's view_target.h). Most recent input wins:
//  - The mouse takes the view back only on real movement (3 pt within 100 ms) or a button.
//  - When the mouse moves while the view shows the caret/focus, the pointer comes to the view
//    (warpPointer); a button press gives the view back without moving the pointer.
//  - Caret/focus changes within a second of a mouse button are the click's own doing, unless a
//    typing key went down after the click.
//  - A caret/focus change takes the view only if a key went down or up within the last second
//    (scrolling moves a caret on screen with no key at all).

public enum ViewOwner: Equatable { case mouse, caret, focus, keys }
public enum TrackKind: Equatable { case none, caret, focus }

public struct TrackSnapshot: Equatable {
    public var kind: TrackKind = .none
    public var seq: UInt = 0
    public var rect = Rc(l: 0, t: 0, r: 0, b: 0)
    public init(kind: TrackKind = .none, seq: UInt = 0, rect: Rc = Rc(l: 0, t: 0, r: 0, b: 0)) {
        self.kind = kind; self.seq = seq; self.rect = rect
    }
}

public struct ViewOwnerState {
    public var owner: ViewOwner = .mouse
    public var lastSeq: UInt = 0
    var moveAccum = 0.0
    var moveWindowMs = 0.0
    public var warpPointer = false
    /// The latched rect the view follows (only from events that passed the gates).
    public var target = TrackSnapshot()
    var wasTracking = true
    public init() {}
}

public struct ViewOwnerInputs {
    public var enabled = false            // zoomed && !locked
    public var trackCaret = false, trackFocus = false
    public var mouseDx = 0.0, mouseDy = 0.0
    public var buttonDown = false
    public var msSinceButton = 1e9
    public var msSinceKey = 0.0
    public var keyAfterButton = false
    public var dtMs = 0.0
    public var snap = TrackSnapshot()
    public var panning = false
    public var trackActive = true
    public init() {}
}

public let kMouseTakeoverPx = 3.0, kMouseTakeoverWindowMs = 100.0
public let kClickQuietMs = 1000.0
public let kKeyDrivenMs = 1000.0

public func resetViewOwnerForSession(_ s: inout ViewOwnerState) {
    s.owner = .mouse
    s.target = TrackSnapshot()
    s.warpPointer = false
    s.moveAccum = 0; s.moveWindowMs = 0
    s.wasTracking = false
}

@discardableResult
public func stepViewOwner(_ s: inout ViewOwnerState, _ i: ViewOwnerInputs) -> ViewOwner {
    s.warpPointer = false
    let detached = s.owner != .mouse
    let tracking = i.enabled && i.trackActive
    if tracking && !s.wasTracking { s.lastSeq = i.snap.seq }
    s.wasTracking = tracking
    if !i.enabled {
        s.owner = .mouse
        s.lastSeq = i.snap.seq
        s.moveAccum = 0; s.moveWindowMs = 0
        return s.owner
    }
    let step = abs(i.mouseDx) + abs(i.mouseDy)
    if step > 0 {
        if s.moveWindowMs > kMouseTakeoverWindowMs { s.moveAccum = 0; s.moveWindowMs = 0 }
        s.moveAccum += step
    }
    s.moveWindowMs += i.dtMs
    if s.moveWindowMs > kMouseTakeoverWindowMs && step == 0 { s.moveAccum = 0; s.moveWindowMs = 0 }
    let moved = s.moveAccum >= kMouseTakeoverPx
    if i.buttonDown || moved {
        s.moveAccum = 0; s.moveWindowMs = 0
        if detached {
            s.owner = .mouse
            s.warpPointer = moved && !i.buttonDown
        }
        s.lastSeq = i.snap.seq
        return s.owner
    }
    if i.panning {
        s.owner = .keys
        s.lastSeq = i.snap.seq
        return s.owner
    }
    if i.trackActive && i.snap.seq != s.lastSeq {
        s.lastSeq = i.snap.seq
        if (i.msSinceButton >= kClickQuietMs || i.keyAfterButton) && i.msSinceKey <= kKeyDrivenMs {
            if i.snap.kind == .caret && i.trackCaret { s.owner = .caret; s.target = i.snap }
            else if i.snap.kind == .focus && i.trackFocus { s.owner = .focus; s.target = i.snap }
        }
    }
    return s.owner
}
