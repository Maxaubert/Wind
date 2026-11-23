// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
import CoreGraphics
import Foundation

/// The menu bar panel's pure parts (Sources/Wind/MenuBarPanel.swift owns the window): where it opens under
/// the status item, when a click on the icon is the same click that just closed it, and the frame-time ring
/// the Performance header reads. Mirrors Windows' flyout_model.h (placement, IgnoreIconClick) and
/// tick_stats.h (the 256-sample ring), with Universal Clipboard's Mac panel lessons (2026-11-17): the card's
/// right edge under the icon's right edge, its top 3 pt under the menu bar (or the icon, when the menu bar
/// hides itself), clamped to the visible frame.
public enum MenuPanel {
    /// The card, as wide as Windows' flyout (kWidth).
    public static let cardWidth: CGFloat = 300
    /// Clear margin left, right and below the card, for its CSS shadow (the window has no native shadow).
    /// There is none above it: macOS keeps a window out of the menu bar, so a clear top edge pushed it down.
    public static let edge: CGFloat = 14
    /// The card's top sits this far under the menu bar.
    public static let gap: CGFloat = 3
    /// A click on the icon this soon after the panel closed is the click that closed it (Windows' kReopenGuardMs).
    public static let reopenGuard: Double = 0.3
    public static var windowWidth: CGFloat { cardWidth + 2 * edge }

    /// The window frame in AppKit screen coordinates (y up). `icon` is the status item button's frame,
    /// `visible` the screen's visibleFrame (below the menu bar, beside the Dock), `height` the page's
    /// reported height (card plus the bottom margin). The card's right edge sits under the icon's right
    /// edge and never leaves the visible frame; a page taller than the screen is cut at the bottom. The top
    /// is under the icon too: with an auto-hiding menu bar (or in a full-screen Space) visibleFrame reaches
    /// the screen top, and the revealed menu bar would cover the card's top.
    public static func frame(icon: CGRect, visible: CGRect, height: CGFloat) -> CGRect {
        let w = windowWidth
        let lo = visible.minX - edge, hi = visible.maxX - cardWidth - edge
        let x = hi < lo ? lo : min(max(icon.maxX - cardWidth - edge, lo), hi)
        let top = min(visible.maxY, icon.minY) - gap
        let h = max(1, min(height, top - visible.minY + edge))
        return CGRect(x: x, y: top - h, width: w, height: h)
    }

    /// The card alone inside a window frame (AppKit coordinates).
    public static func card(in window: CGRect) -> CGRect {
        CGRect(x: window.minX + edge, y: window.minY + edge, width: cardWidth, height: max(0, window.height - edge))
    }

    /// True when an icon click at `now` must be ignored: the panel closed (focus moved to the status
    /// bar as the button went down) under `reopenGuard` seconds ago, so this click is the one that closed it.
    public static func ignoreClick(now: Double, closedAt: Double) -> Bool {
        closedAt > 0 && now >= closedAt && now - closedAt < reopenGuard
    }
}

/// The last 256 display-link tick intervals in ms (Windows' TickStats ring), filled by Magnifier.step on
/// every tick that was not a wake from idle; the panel's Performance header reads it while it is open.
public struct TickRing: Sendable {
    public static let capacity = 256
    private var buf = [Float](repeating: 0, count: TickRing.capacity)
    private var next = 0
    public private(set) var count = 0

    public init() {}

    public mutating func push(_ ms: Float) {
        guard ms.isFinite, ms > 0 else { return }
        buf[next] = ms
        next = (next + 1) % TickRing.capacity
        count = min(count + 1, TickRing.capacity)
    }

    /// The samples, oldest first.
    public var samples: [Float] {
        if count < TickRing.capacity { return Array(buf[0..<count]) }
        return Array(buf[next...] + buf[..<next])
    }

    public mutating func clear() { next = 0; count = 0 }
}
