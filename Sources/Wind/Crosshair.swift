import AppKit

// Inspect mode's crosshair: a click-through window at the view centre. It lives on the desktop
// like any window, so the compositor zoom magnifies it; it is sized by 1/level to look the same
// at every zoom.

final class Crosshair {
    private var panel: NSPanel?
    private let armPt = 26.0, gapPt = 6.0, linePt = 2.0      // as seen on screen

    private final class View: NSView {
        var level = 1.0
        var arm = 26.0, gap = 6.0, line = 2.0
        override func draw(_ r: NSRect) {
            let c = NSPoint(x: bounds.midX, y: bounds.midY)
            let a = arm / level, g = gap / level, w = line / level
            for (color, width) in [(NSColor.black.withAlphaComponent(0.6), w * 2.2), (NSColor.white, w)] {
                color.setStroke()
                let p = NSBezierPath(); p.lineWidth = width
                p.move(to: NSPoint(x: c.x - a, y: c.y)); p.line(to: NSPoint(x: c.x - g, y: c.y))
                p.move(to: NSPoint(x: c.x + g, y: c.y)); p.line(to: NSPoint(x: c.x + a, y: c.y))
                p.move(to: NSPoint(x: c.x, y: c.y - a)); p.line(to: NSPoint(x: c.x, y: c.y - g))
                p.move(to: NSPoint(x: c.x, y: c.y + g)); p.line(to: NSPoint(x: c.x, y: c.y + a))
                p.stroke()
            }
        }
    }

    /// Show at a desktop point (global, top-left origin) for a zoom level; nil hides it.
    func show(at p: CGPoint?, level: Double) {
        guard let p else { panel?.orderOut(nil); return }
        let panel = self.panel ?? make()
        let size = (armPt + 4) * 2 / max(1, level)
        let primaryH = NSScreen.screens.first?.frame.height ?? 0
        panel.setFrame(NSRect(x: p.x - size / 2, y: primaryH - p.y - size / 2, width: size, height: size), display: false)
        if let v = panel.contentView as? View, v.level != level { v.level = level; v.needsDisplay = true }
        if !panel.isVisible { panel.orderFrontRegardless() }
        CATransaction.flush()                                // commit the move now, before the zoom write
    }

    private func make() -> NSPanel {
        let p = NSPanel(contentRect: .zero, styleMask: [.borderless, .nonactivatingPanel], backing: .buffered, defer: false)
        p.isOpaque = false
        p.backgroundColor = .clear
        p.hasShadow = false
        p.ignoresMouseEvents = true
        p.level = .screenSaver
        p.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary, .stationary, .ignoresCycle]
        let v = View()
        v.arm = armPt; v.gap = gapPt; v.line = linePt
        p.contentView = v
        panel = p
        return p
    }
}
