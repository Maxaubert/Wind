import AppKit

// Inspect mode's crosshair at the view centre. Like Wind's own cursor it is a layer in a
// click-through window that covers the screen and never moves (a moved window reached the screen
// 1-2 frames after the zoom write: the crosshair jumped whole points times the level while
// panning, measured on the zoomed screen 2026-10-31). The compositor zoom magnifies it, so it is
// drawn 1/level in size to look the same at every zoom. It moves before each view write
// (`move(to:level:)`), committed first, so both reach the same frame.

final class Crosshair {
    private var panel: NSPanel?
    private let armPt = 26.0, gapPt = 6.0, linePt = 2.0      // as seen on screen
    private let root = CALayer()
    private let outline = CAShapeLayer(), line = CAShapeLayer()
    private var pathLevel = 0.0
    /// Where it is drawn now (desktop point, top-left origin), nil while hidden.
    private(set) var drawnAt: CGPoint?
    #if WIND_TEST_CHANNEL
    /// Test builds: a magenta block at the centre too, for the zoomed-screen capture.
    static let probe = ProcessInfo.processInfo.environment["WIND_CURSOR_PROBE"] != nil
    #else
    static let probe = false
    #endif

    /// Show at a desktop point (global, top-left origin) for a zoom level; nil hides it.
    func show(at p: CGPoint?, level: Double) {
        guard let p else { panel?.orderOut(nil); drawnAt = nil; return }
        let panel = self.panel ?? make()
        if let d = drawnAt, panel.isVisible, d == p, pathLevel == max(1, level) { return }
        place(p, level: level, panel: panel)
        if !panel.isVisible { panel.orderFrontRegardless() }
        CATransaction.flush()                                // commit the move now, before the zoom write
    }

    /// Move it only (it must be showing), committed at once: just before a view write.
    func move(to p: CGPoint, level: Double) {
        guard let panel, panel.isVisible, let d = drawnAt else { return }
        if d == p && pathLevel == max(1, level) { return }
        place(p, level: level, panel: panel)
        CATransaction.flush()
        if Crosshair.probe { Log.verbose(String(format: "cm %.6f %.2f %.2f", CACurrentMediaTime(), p.x, p.y)) }
    }

    private func place(_ p: CGPoint, level: Double, panel: NSPanel) {
        drawnAt = p
        let primaryH = NSScreen.screens.first?.frame.height ?? 0
        let q = NSPoint(x: p.x, y: primaryH - p.y)
        let f = (NSScreen.screens.first { NSMouseInRect(q, $0.frame, false) } ?? NSScreen.screens.first)?.frame
            ?? NSRect(x: 0, y: 0, width: 1, height: 1)
        if panel.frame != f { panel.setFrame(f, display: false) }
        CATransaction.begin(); CATransaction.setDisableActions(true)
        let lv = max(1, level)
        if lv != pathLevel { pathLevel = lv; setPaths(lv) }
        root.position = CGPoint(x: p.x - f.minX, y: f.height - (p.y - (primaryH - f.maxY)))
        CATransaction.commit()
    }

    private func setPaths(_ lv: Double) {
        let a = armPt / lv, g = gapPt / lv, w = linePt / lv
        let path = CGMutablePath()
        path.move(to: CGPoint(x: -a, y: 0)); path.addLine(to: CGPoint(x: -g, y: 0))
        path.move(to: CGPoint(x: g, y: 0)); path.addLine(to: CGPoint(x: a, y: 0))
        path.move(to: CGPoint(x: 0, y: -a)); path.addLine(to: CGPoint(x: 0, y: -g))
        path.move(to: CGPoint(x: 0, y: g)); path.addLine(to: CGPoint(x: 0, y: a))
        outline.path = path; outline.lineWidth = w * 2.2
        line.path = path; line.lineWidth = w
        if Crosshair.probe {                                 // 12 x 12 screen points at the centre
            if probe.superlayer == nil { probe.backgroundColor = CGColor(srgbRed: 1, green: 0, blue: 1, alpha: 1); root.insertSublayer(probe, at: 0) }
            probe.frame = CGRect(x: -6 / lv, y: -6 / lv, width: 12 / lv, height: 12 / lv)
        }
    }
    private let probe = CALayer()

    private func make() -> NSPanel {
        let p = NSPanel(contentRect: .zero, styleMask: [.borderless, .nonactivatingPanel], backing: .buffered, defer: false)
        p.isOpaque = false
        p.backgroundColor = .clear
        p.hasShadow = false
        p.animationBehavior = .none                  // shown and hidden at once, no fade
        p.ignoresMouseEvents = true
        p.level = .screenSaver
        p.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary, .stationary, .ignoresCycle]
        let v = NSView()
        v.wantsLayer = true
        outline.strokeColor = NSColor.black.withAlphaComponent(0.6).cgColor
        line.strokeColor = NSColor.white.cgColor
        for l in [outline, line] { l.fillColor = nil; l.lineCap = .butt; root.addSublayer(l) }
        root.bounds = .zero
        v.layer?.addSublayer(root)
        p.contentView = v
        SkyLight.ignoreForWindowPicker(windowNumber: p.windowNumber)
        panel = p
        return p
    }
}
