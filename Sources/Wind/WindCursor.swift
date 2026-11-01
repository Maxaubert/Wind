import AppKit

// Wind's own cursor for macOS's zoom (config cursor = "wind"). The zoom shows whole desktop
// points, so the real cursor sits up to one point (the level in pixels) off where a fractional
// view would put it, and that offset changes as the pointer's fraction does: the cursor wobbles
// around the view centre while panning and zooming. Wind hides the real cursor and draws the current
// one where it would be on a fractional view: steady on screen while the view follows it. Clicks
// still land at the real pointer, under one desktop point away.
// It is an image layer in a click-through window that covers the screen and never moves; the zoom
// magnifies it like any window, so it looks as big as the real one. Before every view write the
// layer is moved to match the write and the move is committed and taken by WindowServer first, so
// the cursor and the zoom change reach the same frame (measured on the zoomed screen with
// scripts/cursor_truth.py, see docs/architecture.md).

final class WindCursor {
    private var panel: NSPanel?
    private let image = CALayer()
    private var lastCheck = 0.0
    private var key: Data?
    private var hotSpot = CGPoint.zero
    private var size = CGSize(width: 1, height: 1)
    private var pendingShape: CGImage??
    private(set) var active = false
    /// Where the cursor is drawn now (desktop point, top-left origin), nil while hidden.
    private(set) var drawnAt: CGPoint?
    #if WIND_TEST_CHANNEL
    /// Test builds: a magenta block instead of the cursor image, for the zoomed-screen capture.
    static let probe = ProcessInfo.processInfo.environment["WIND_CURSOR_PROBE"] != nil
    /// Test builds: the old small window moved to the cursor (for comparison on the capture).
    static let moveWindow = ProcessInfo.processInfo.environment["WIND_CURSOR_MOVEWINDOW"] != nil
    #else
    static let probe = false
    static let moveWindow = false
    #endif
    /// Wait for WindowServer to take each commit before the zoom write.
    nonisolated(unsafe) static var syncCommit = false
    /// Move the cursor before every view write of a frame (carries and level writes come before the
    /// frame's last write), not only once per frame.
    nonisolated(unsafe) static var preCommit = true

    /// Show the cursor at a desktop point (global, top-left origin, fractional); nil hides it.
    /// `on` keeps the real cursor hidden (Wind's cursor is in use, even while it is not drawn).
    func show(at p: CGPoint?, on: Bool) {
        if on != active {
            active = on
            SkyLight.forceHidden = on
            if on { SkyLight.hideRealCursor() } else { SkyLight.restoreWanted() }
        }
        guard on, let p else { panel?.orderOut(nil); drawnAt = nil; return }
        let panel = self.panel ?? make()
        let now = CACurrentMediaTime()
        if now - lastCheck > 0.1 {                      // the shape (arrow, I-beam, ...) changes rarely
            lastCheck = now
            SkyLight.hideRealCursor()                   // macOS can show it again (Dock, app switch)
            if WindCursor.probe {
                if key == nil {
                    key = Data([1]); size = CGSize(width: 12, height: 12); hotSpot = CGPoint(x: 6, y: 6)
                    image.contents = nil; image.backgroundColor = CGColor(srgbRed: 1, green: 0, blue: 1, alpha: 1)
                }
            } else if let cur = NSCursor.currentSystem {
                let img = cur.image
                let k = img.tiffRepresentation          // same size and hotspot can be another cursor
                if k != key {
                    key = k; size = img.size; hotSpot = cur.hotSpot
                    var r = CGRect(origin: .zero, size: img.size)
                    var cg = img.cgImage(forProposedRect: &r, context: nil, hints: nil)
                    #if WIND_TEST_CHANNEL
                    // WIND_CURSOR_SHAPE_PROBE: the shape as a magenta silhouette, for scripts/cursor_shape_truth.py.
                    if ProcessInfo.processInfo.environment["WIND_CURSOR_SHAPE_PROBE"] != nil, let src = cg,
                       let ctx = CGContext(data: nil, width: src.width, height: src.height, bitsPerComponent: 8, bytesPerRow: 0,
                                           space: CGColorSpaceCreateDeviceRGB(), bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue) {
                        let rect = CGRect(x: 0, y: 0, width: src.width, height: src.height)
                        ctx.clip(to: rect, mask: src)
                        ctx.setFillColor(CGColor(red: 1, green: 0, blue: 1, alpha: 1)); ctx.fill(rect)
                        cg = ctx.makeImage()
                    }
                    #endif
                    pendingShape = .some(cg)
                }
            }
        }
        if pendingShape == nil, let d = drawnAt, panel.isVisible, abs(d.x - p.x) < 1e-9, abs(d.y - p.y) < 1e-9 { return }
        place(p, panel: panel)
        if !panel.isVisible { panel.orderFrontRegardless() }
        commit()
    }

    /// Move the drawn cursor only (it must be showing) and commit at once: called just before a
    /// view write, so both reach the screen in the same frame.
    func move(to p: CGPoint) {
        guard WindCursor.preCommit, active, let panel, panel.isVisible, let d = drawnAt, !WindCursor.moveWindow else { return }
        if abs(d.x - p.x) < 1e-9 && abs(d.y - p.y) < 1e-9 { return }
        place(p, panel: panel)
        commit()
    }

    private func commit() {
        CATransaction.flush()                           // send the move now, before the zoom write
        // ...and wait until WindowServer has taken it. Without this the zoom write (a plain message,
        // applied at once) sometimes reached the screen a frame before the layer move.
        if WindCursor.syncCommit { _ = (CATransaction.self as AnyObject).perform(NSSelectorFromString("synchronize")) }
    }

    private func place(_ p: CGPoint, panel: NSPanel) {
        drawnAt = p
        let primaryH = NSScreen.screens.first?.frame.height ?? 0
        CATransaction.begin(); CATransaction.setDisableActions(true)
        // A new shape goes up in the same no-animation commit as its size and place: set on its own,
        // Core Animation animated it and the cursor showed big at every shape change (owner
        // 2026-11-01, render's bug of 2026-10-31; scripts/cursor_shape_truth.py --engine zoom).
        if let c = pendingShape { image.contents = c; pendingShape = nil }
        if WindCursor.moveWindow {
            // The old way: a small window moved to the cursor, the image carrying the fraction.
            // Measured on the zoomed screen: the window move reached the screen 1-2 frames after the
            // zoom write, so the cursor jumped whole points times the level while panning.
            let ox = floor(p.x - hotSpot.x) - 1, oy = floor(p.y - hotSpot.y) - 1   // top-left, desktop
            let w = ceil(size.width) + 3, h = ceil(size.height) + 3
            panel.setFrame(NSRect(x: ox, y: primaryH - oy - h, width: w, height: h), display: false)
            let ix = p.x - hotSpot.x - ox, iy = p.y - hotSpot.y - oy                // inside, top-left
            image.frame = CGRect(x: ix, y: h - iy - size.height, width: size.width, height: size.height)
        } else {
            // The window covers the screen and never moves; only the image layer moves.
            let f = screenFrame(for: p, primaryH: primaryH)
            if panel.frame != f { panel.setFrame(f, display: false) }
            let top = primaryH - f.maxY, left = f.minX                          // panel's top-left, desktop
            let ix = p.x - hotSpot.x - left, iy = p.y - hotSpot.y - top
            image.frame = CGRect(x: ix, y: f.height - iy - size.height, width: size.width, height: size.height)
        }
        CATransaction.commit()
    }

    /// The frame (AppKit coordinates) of the screen holding a desktop point (top-left origin).
    private func screenFrame(for p: CGPoint, primaryH: CGFloat) -> NSRect {
        let q = NSPoint(x: p.x, y: primaryH - p.y)
        let s = NSScreen.screens.first { NSMouseInRect(q, $0.frame, false) } ?? NSScreen.screens.first
        return s?.frame ?? NSRect(x: 0, y: 0, width: 1, height: 1)
    }

    /// The cursor image as WindowServer has it (tests): top-left origin desktop bounds of the image,
    /// from the window's server bounds plus the layer's place in it; nil when not on screen.
    var serverBounds: CGRect? {
        guard let panel, panel.isVisible,
              let info = CGWindowListCopyWindowInfo(.optionIncludingWindow, CGWindowID(panel.windowNumber)) as? [[String: Any]],
              let d = info.first?[kCGWindowBounds as String] as? NSDictionary,
              let b = CGRect(dictionaryRepresentation: d) else { return nil }
        let f = image.frame
        return CGRect(x: b.minX + f.minX, y: b.minY + (b.height - f.maxY), width: f.width, height: f.height)
    }

    private func make() -> NSPanel {
        let p = NSPanel(contentRect: .zero, styleMask: [.borderless, .nonactivatingPanel], backing: .buffered, defer: false)
        p.isOpaque = false
        p.backgroundColor = .clear
        p.hasShadow = false
        p.animationBehavior = .none                  // shown and hidden at once, no fade
        p.ignoresMouseEvents = true
        p.level = NSWindow.Level(rawValue: NSWindow.Level.screenSaver.rawValue + 1)   // above the crosshair
        p.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary, .stationary, .ignoresCycle]
        let v = NSView()
        v.wantsLayer = true
        image.contentsGravity = .resize
        image.minificationFilter = .linear
        image.magnificationFilter = .linear
        v.layer?.addSublayer(image)
        p.contentView = v
        panel = p
        return p
    }
}
