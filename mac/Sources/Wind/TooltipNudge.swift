// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
import AppKit

/// AppKit tooltips while the real cursor is hidden for motion (ClearCursor, owner 2026-11-06: no tooltips in
/// Xcode while zoomed). `-[NSToolTipManager mouseEnteredToolTip:inWindow:pointInWindow:]` asks
/// `+[NSCursor _isHidden]` once, when the pointer enters the tooltip's rect, and starts no timer if hidden; later
/// moves never start one. Every entry happens mid-motion, so no tooltip ever came. At rest, if the element under
/// the pointer has a tooltip (AXHelp) in the frontmost app, a tiny near-transparent window sits under the pointer
/// for `flashMs`: the app sees the pointer leave and come back with the cursor shown, and the tooltip starts.
/// Once per visit: no flash again for the same element until the pointer leaves its frame (a slow hand stops
/// every few points; each flash restarts the tooltip's timer and blinks the hover).
/// Cost: that element's hover blinks off for one or two frames. One AX look per stop, no polling.
final class TooltipNudge: @unchecked Sendable {
    static let shared = TooltipNudge()
    /// 20 ms brought the tooltip in 2 of 2 stops, 12 in 3 of 4, 8 in 1 of 4 (WindowServer re-tests the window
    /// under the pointer on its own schedule); 25 for margin.
    static let flashMs = 25.0
    static let enabled: Bool = {
        #if WIND_TEST_CHANNEL
        if ProcessInfo.processInfo.environment["WIND_TIP_NUDGE"] == "0" { return false }
        #endif
        return true
    }()
    private let queue = DispatchQueue(label: "wind.tooltipnudge", qos: .userInitiated)
    private let system: AXUIElement = {
        let s = AXUIElementCreateSystemWide(); AXUIElementSetMessagingTimeout(s, 0.25); return s
    }()
    private var busy = false                            // on `queue`: one look at a time
    private var panel: NSPanel?                         // main thread
    private(set) var flashes = 0                        // main thread (test state)
    private let lock = NSLock()
    private var visited: (el: AXUIElement, frame: CGRect)?   // under lock: flashed, the pointer still inside

    /// Main thread (the event tap), every hand move: leaving the flashed element's frame re-arms it.
    func moved(_ p: CGPoint) {
        lock.lock(); if let v = visited, !v.frame.contains(p) { visited = nil }; lock.unlock()
    }

    /// Any thread, once per stop after a hidden motion.
    func rested(isMoving: @escaping @Sendable () -> Bool) {
        guard TooltipNudge.enabled else { return }
        queue.async { [self] in
            if busy { return }
            guard let at = CGEvent(source: nil)?.location, let front = NSWorkspace.shared.frontmostApplication?.processIdentifier,
                  front != getpid() else { return }
            busy = true; defer { busy = false }
            guard let el = tooltipElement(at: at, front: front), !isMoving(), CGEvent(source: nil)?.location == at else { return }
            lock.lock(); let same = visited.map { CFEqual($0.el, el) } ?? false; lock.unlock()
            if same { return }
            let frame = frameOf(el) ?? CGRect(x: at.x - 1, y: at.y - 1, width: 2, height: 2)
            DispatchQueue.main.async { [self] in
                guard !isMoving(), CGEvent(source: nil)?.location == at else { return }
                lock.lock(); visited = (el, frame); lock.unlock()
                flash()
            }
        }
    }

    /// The element under the pointer (or one of its first ancestors) with tooltip text, in the frontmost app:
    /// AppKit shows tooltips only for the active app.
    private func tooltipElement(at p: CGPoint, front: pid_t) -> AXUIElement? {
        var hit: AXUIElement?
        guard AXUIElementCopyElementAtPosition(system, Float(p.x), Float(p.y), &hit) == .success, var el = hit else { return nil }
        var pid: pid_t = 0
        guard AXUIElementGetPid(el, &pid) == .success, pid == front else { return nil }
        for _ in 0..<3 {
            var v: CFTypeRef?
            if AXUIElementCopyAttributeValue(el, kAXHelpAttribute as CFString, &v) == .success,
               let s = v as? String, !s.isEmpty { return el }
            var up: CFTypeRef?
            guard AXUIElementCopyAttributeValue(el, kAXParentAttribute as CFString, &up) == .success, let u = up,
                  CFGetTypeID(u) == AXUIElementGetTypeID() else { return nil }
            el = u as! AXUIElement
        }
        return nil
    }

    /// The element's frame in desktop points, top left (the event tap's coordinates while ClearCursor runs).
    private func frameOf(_ el: AXUIElement) -> CGRect? {
        var pv: CFTypeRef?, sv: CFTypeRef?
        guard AXUIElementCopyAttributeValue(el, kAXPositionAttribute as CFString, &pv) == .success,
              AXUIElementCopyAttributeValue(el, kAXSizeAttribute as CFString, &sv) == .success,
              let pv, let sv, CFGetTypeID(pv) == AXValueGetTypeID(), CFGetTypeID(sv) == AXValueGetTypeID() else { return nil }
        var o = CGPoint.zero, z = CGSize.zero
        guard AXValueGetValue(pv as! AXValue, .cgPoint, &o), AXValueGetValue(sv as! AXValue, .cgSize, &z) else { return nil }
        return CGRect(origin: o, size: z)
    }

    private func flash() {
        let p = NSEvent.mouseLocation
        let w = panel ?? {
            let w = NSPanel(contentRect: .zero, styleMask: [.borderless, .nonactivatingPanel], backing: .buffered, defer: true)
            w.backgroundColor = NSColor(white: 0, alpha: 0.02); w.isOpaque = false; w.hasShadow = false
            w.level = .screenSaver; w.ignoresMouseEvents = false; w.sharingType = .none
            w.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary, .stationary, .ignoresCycle]
            w.isReleasedWhenClosed = false; w.hidesOnDeactivate = false
            panel = w; return w
        }()
        w.setFrame(NSRect(x: p.x - 2, y: p.y - 2, width: 4, height: 4), display: false)
        w.orderFrontRegardless()
        flashes += 1
        DispatchQueue.main.asyncAfter(deadline: .now() + TooltipNudge.flashMs / 1000) { w.orderOut(nil) }
    }
}
