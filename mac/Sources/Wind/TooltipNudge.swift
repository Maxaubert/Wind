// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
import AppKit

/// AppKit tooltips while the real cursor is hidden for motion (ClearCursor, owner 2026-11-06: no tooltips in
/// Xcode while zoomed). `-[NSToolTipManager mouseEnteredToolTip:inWindow:pointInWindow:]` asks
/// `+[NSCursor _isHidden]` once, when the pointer enters the tooltip's rect, and starts no timer if hidden; later
/// moves never start one. Every entry happens mid-motion, so no tooltip ever came. At every rest a tiny
/// near-transparent window sits under the pointer for `flashMs`: the app sees the pointer leave and come back
/// with the cursor shown, and a tooltip there starts. At once, with no Accessibility look first (owner
/// 2026-11-07: no flicker seen, so faster beats skipping elements without a tooltip). No polling.
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
    private var panel: NSPanel?                         // main thread
    private(set) var flashes = 0                        // main thread (test state)

    /// Any thread, once per stop after a hidden motion.
    func rested(isMoving: @escaping @Sendable () -> Bool) {
        guard TooltipNudge.enabled else { return }
        DispatchQueue.main.async { [self] in
            guard !isMoving(), NSWorkspace.shared.frontmostApplication?.processIdentifier != getpid() else { return }
            flash()
        }
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
