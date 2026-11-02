import AppKit
import CoreMedia
import ScreenCaptureKit

// Experimental draw engine (config engine = "draw", issue #8). Instead of zooming macOS, Wind
// captures the screen with ScreenCaptureKit and draws it magnified in a click-through overlay above
// everything, with the real cursor hidden and the current system cursor drawn by Wind.
// - Panning is sub-pixel: the captured image is placed at any fractional offset (the compositor
//   zoom only takes whole desktop points).
// - macOS is never zoomed, so the pointer moves at its own 1x speed: no speed compensation, no
//   placement carries, warps are exact.
// - The overlay is left out of its own capture (by window), so ordinary screenshots still show it.
// Costs: about one frame of extra content latency (measured: frames reach Wind 1-2 ms after
// WindowServer composites them), Screen Recording permission, more GPU work.
//
// Safety (review 2026-10-31): the overlay hides the real screen, so it must never stay up frozen.
// A stopped stream is restarted once, then the engine falls back to macOS's zoom; no first frame
// within 1.5 s falls back too; more than one display falls back (not supported yet); a display
// change resets the overlay; and a watchdog quits Wind if its main thread stops answering while the
// overlay is up (the overlay and the cursor hiding die with the process).

final class DrawZoom: NSObject, SCStreamOutput, SCStreamDelegate, @unchecked Sendable {
    static let shared = DrawZoom()
    /// On when the config selects the draw engine.
    nonisolated(unsafe) static var enabled = false
    /// The engine failed this run (no permission, capture stopped, several displays): Wind stays on
    /// macOS's zoom until it restarts.
    nonisolated(unsafe) static var failed = false
    /// Called on the main thread after a failure switched the engine off.
    var onFailure: (() -> Void)?
    /// Called on the main thread when a system prompt makes Wind hand the zoom to macOS (true) or
    /// take it back (false).
    var onHandover: ((Bool) -> Void)?
    /// A system prompt is up: macOS's zoom shows it (prompts sit above the overlay or cannot be
    /// captured, so they showed twice or not at all). The config still selects the draw engine.
    nonisolated(unsafe) static var handedOver = false
    private var promptTimer: Timer?
    private var handoverWindow: Int?           // the window that made the engine hand over

    private var window: NSWindow?
    private let image = CALayer()
    private let cursorLayer = CALayer()
    // Inspect's crosshair, drawn in the overlay (screen points). As a window of its own on the desktop it
    // was captured and drawn magnified a capture frame or more after the view moved: it jumped on 21-22%
    // of frames while panning, up to 12.7 pt (cursor_truth inspect on render, 2026-11-01).
    private let crossRoot = CALayer(), crossOutline = CAShapeLayer(), crossLine = CAShapeLayer(), crossProbe = CALayer()
    /// Where Inspect's crosshair is (desktop point), nil when not in Inspect. Drawn with the view.
    var crosshair: CGPoint?
    /// Wind's own screenshot selection (WindShot), drawn with the cursor while it is up.
    var shot: WindShot?
    private let shotLayers = ShotLayers()
    private var stream: SCStream?
    private var generation = 0                       // frames from older streams are dropped
    private var starting = false
    private var restarts = 0
    private(set) var visible = false
    private(set) var center = CGPoint(x: 0, y: 0)
    private(set) var scale = 1.0
    private var smooth = false
    private var haveFrame = false
    private var shownAt = 0.0
    private var latest: IOSurface?                    // the newest captured frame (kept while warm)
    private var idleStop: DispatchWorkItem?
    private var lastCursorCheck = 0.0
    private var cursorKey: Data?
    private var cursorHotSpot = CGPoint.zero
    private var cursorSize = CGSize.zero
    private var observing = false
    /// Frames shown since the overlay appeared (tests and logs).
    private(set) var frames = 0
    /// Wind wants the pointer hidden (caret tracking, Inspect, the hide-cursor bind).
    var pointerHidden = false
    /// A game holds the pointer: no drawn cursor either.
    var lockHidden = false
    /// Main-thread heartbeat for the hang watchdog.
    nonisolated(unsafe) private static var beat = CACurrentMediaTime()
    nonisolated(unsafe) private static var overlayUp = false

    /// The view: centre in desktop points (top-left origin, fractional), scale > 1 shows it.
    func set(center c: CGPoint, scale s: Double, smooth sm: Bool) {
        if s <= 1.0001 { hide(); return }
        if NSScreen.screens.count > 1 { fail("more than one display (not supported yet)"); return }
        center = c; scale = s; smooth = sm
        if window == nil { makeWindow() }
        if !visible {
            visible = true
            DrawZoom.overlayUp = true
            DrawZoom.beat = CACurrentMediaTime()
            startWatchdog()
            idleStop?.cancel(); idleStop = nil
            // A warm stream already has the screen: show it at once (a cold start takes ~80 ms).
            haveFrame = latest != nil
            pendingSurface = nil
            if let l = latest { CATransaction.begin(); CATransaction.setDisableActions(true); image.contents = l; CATransaction.commit() }
            window?.alphaValue = haveFrame ? 1 : 0      // else shown with the first captured frame
            layout()                                    // the right scale before the window is ordered in
            // The real cursor goes when the picture comes: hidden over a still-transparent overlay it
            // left the unzoomed screen with no cursor for 90-136 ms on a cold start (2026-10-31).
            // Cursor shape and hiding come BEFORE the order-in: right after it, CGDisplayHideCursor
            // and NSCursor.currentSystem each waited 2-8 ms on WindowServer in the first zoom tick.
            if haveFrame { CATransaction.begin(); CATransaction.setDisableActions(true); cursorShape(); CATransaction.commit(); lastCursorCheck = CACurrentMediaTime(); takeCursor() }
            window?.orderFrontRegardless()
            // Above password prompts too: they are captured like any window and then drawn magnified
            // once, and clicks land on them where drawn (the drawn cursor maps onto the real pointer).
            // Off the main thread: the two WindowServer calls took 3-6 ms in the first zoom tick, which
            // then came 11-14 ms late (2026-10-31).
            if let n = window?.windowNumber {
                DrawZoom.placeQueue.async {
                    let ok = SkyLight.placeAbovePrompts(windowNumber: n)
                    DispatchQueue.main.async { DrawZoom.abovePrompts = ok }
                }
            }
            shownAt = CACurrentMediaTime()
            if stream == nil { startStream() }
            startPromptWatch()
            // Zoomed in during a screenshot or sampler mode: hand over right away, not at the next poll.
            if PointerTakeover.shared.active { DispatchQueue.main.async { self.checkPrompts() } }
            let gen = generation
            DispatchQueue.main.asyncAfter(deadline: .now() + 1.5) { [weak self] in
                guard let self, self.visible, !self.haveFrame, self.generation == gen else { return }
                self.fail("no captured frame within 1.5 s")
            }
        }
        layout()
    }

    /// The overlay shows a picture: hide the real cursor, Wind draws it from now on.
    private func takeCursor() {
        guard !SkyLight.forceHidden else { return }
        pointerHidden = SkyLight.wantHidden             // carry Wind's own hide state over
        SkyLight.forceHidden = true
        SkyLight.hideRealCursor()
        cursorTakenAt = CACurrentMediaTime()
    }
    private var cursorTakenAt = 0.0
    private var lastTickAt = 0.0                      // the last updateCursor (one per zoom tick)
    private var pendingSurface: IOSurface?            // a captured frame for the next tick's commit

    /// A captured frame is at hand, so the overlay can show the zoom from its first frame.
    var hasFrame: Bool { latest != nil }

    /// Cold start (render engine): start capturing before the zoom moves, overlay still ordered out.
    /// Magnifier holds the level at 1x until `hasFrame`, so the first zoomed frame is at ~1.0004
    /// instead of popping in at 1.05-1.14 after the stream's 90-136 ms start (measured 2026-10-31).
    func prewarm() {
        guard !visible else { return }
        if stream == nil && !starting {
            if NSScreen.screens.count > 1 { fail("more than one display (not supported yet)"); return }
            if window == nil { makeWindow() }
            scheduleIdleStop()                          // also lets startStream keep the stream
            startStream()
            warmingSince = CACurrentMediaTime()
        }
        // Whenever the zoom waits for a first frame, it waits at most 1.5 s (also for a stream that
        // already exists without one: review 2026-10-31).
        guard latest == nil, !noFrameArmed else { return }
        noFrameArmed = true
        let gen = generation
        DispatchQueue.main.asyncAfter(deadline: .now() + 1.5) { [weak self] in
            guard let self else { return }
            self.noFrameArmed = false
            guard self.latest == nil, self.generation == gen, DrawZoom.enabled, !self.visible else { return }
            self.fail("no captured frame within 1.5 s")
        }
    }
    private var warmingSince = 0.0
    private var noFrameArmed = false

    private func scheduleIdleStop() {
        idleStop?.cancel()
        // Keep capturing for a minute so the next zoom-in shows at once; then stop (power, and the
        // screen-recording indicator).
        let stop = DispatchWorkItem { [weak self] in
            guard let self, !self.visible else { return }
            self.stopStream()
            Log.info("draw engine: capture stopped after a minute at 1x")
        }
        idleStop = stop
        DispatchQueue.main.asyncAfter(deadline: .now() + 60, execute: stop)
    }

    func hide() {
        guard visible else { return }
        visible = false
        DrawZoom.overlayUp = false
        window?.orderOut(nil)
        SkyLight.forceHidden = false
        SkyLight.restoreWanted()                        // the real cursor as Wind wants it at 1x
        scheduleIdleStop()
        Log.info("draw engine: off after \(frames) frames")
        frames = 0
    }

    /// The overlay's window level: above every app and almost every prompt; below the lock-screen
    /// shield. (Password and Touch ID prompts, SecurityAgent and coreautha, are stacked above any
    /// app window whatever its level: measured 2026-10-31 at the maximum level and with every
    /// SkyLight tag bit. Only macOS's zoom can magnify them.)
    static let overlayLevel = Int(CGShieldingWindowLevel()) - 1

    /// Owners of the big windows WindowServer stacks above the overlay (tests).
    static func ownersAboveOverlay() -> [String] {
        guard let list = CGWindowListCopyWindowInfo([.optionOnScreenOnly, .excludeDesktopElements], kCGNullWindowID) as? [[String: Any]] else { return [] }
        let me = ProcessInfo.processInfo.processIdentifier
        var out: [String] = []
        for w in list {
            if (w[kCGWindowOwnerPID as String] as? Int32) == me && (w[kCGWindowLayer as String] as? Int) == overlayLevel { return out }
            let b = w[kCGWindowBounds as String] as? [String: Double] ?? [:]
            if (b["Width"] ?? 0) > 60 && (b["Height"] ?? 0) > 40 { out.append(w[kCGWindowOwnerName as String] as? String ?? "?") }
        }
        return []                                       // the overlay is not on screen
    }

    /// Serial queue for SkyLight.placeAbovePrompts (its space is created once, on this queue).
    static let placeQueue = DispatchQueue(label: "wind.draw.place", qos: .userInteractive)
    /// The overlay sits on a space above password prompts (SkyLight.placeAbovePrompts).
    nonisolated(unsafe) static var abovePrompts = false

    /// The lock screen and screen saver: not captured, so macOS's zoom shows them.
    private static let lockOwners: Set<String> = ["loginwindow", "ScreenSaverEngine"]

    /// Whether something is on screen the drawn zoom cannot show right: a window stacked above the
    /// overlay (it would show twice, unzoomed on top of its magnified copy), a window that opts out
    /// of screen capture (it would be hidden), or the lock screen. Every other window, prompts
    /// included, is captured and drawn magnified, and clicks land on it where it is drawn (the
    /// Screen Recording prompt, universalAccessAuthWarn, sits under the overlay: it stays drawn).
    /// The window that makes it so (its window number), or nil.
    static func blockingWindow() -> Int? {
        guard let list = CGWindowListCopyWindowInfo([.optionOnScreenOnly, .excludeDesktopElements], kCGNullWindowID) as? [[String: Any]] else { return nil }
        let me = ProcessInfo.processInfo.processIdentifier
        var ignore = Set<String>()
        #if WIND_TEST_CHANNEL
        // Tests can ignore a real window left on screen (owner away).
        if let v = ProcessInfo.processInfo.environment["WIND_IGNORE_PROMPTS"] { ignore = Set(v.split(separator: ",").map(String.init)) }
        #endif
        var aboveOverlay = true                         // the list runs front to back
        let overlayShown = list.contains { ($0[kCGWindowOwnerPID as String] as? Int32) == me && ($0[kCGWindowLayer as String] as? Int) == overlayLevel }
        for w in list {
            let layer = w[kCGWindowLayer as String] as? Int ?? 0
            if (w[kCGWindowOwnerPID as String] as? Int32) == me {
                if layer == overlayLevel { aboveOverlay = false }
                continue
            }
            let owner = w[kCGWindowOwnerName as String] as? String ?? ""
            let alpha = w[kCGWindowAlpha as String] as? Double ?? 1
            let sharing = w[kCGWindowSharingState as String] as? Int ?? 1
            let b = w[kCGWindowBounds as String] as? [String: Double] ?? [:]
            let big = (b["Width"] ?? 0) > 60 && (b["Height"] ?? 0) > 40
            if alpha < 0.05 || !big || owner == "Window Server" { continue }   // the recording indicator is 28x28
            if ignore.contains(owner) { continue }
            let id = w[kCGWindowNumber as String] as? Int ?? 0
            if lockOwners.contains(owner) || sharing == 0 { return id }
            // Above the overlay (listed before it). With the overlay not on screen, judge by level:
            // prompts at or above the screen-saver level would stack over it, unless the overlay
            // goes on its own space above them.
            if aboveOverlay && (!overlayShown ? !abovePrompts && layer >= NSWindow.Level.screenSaver.rawValue : true) { return id }
        }
        return nil
    }

    /// Is this window still on screen?
    static func windowOnScreen(_ id: Int) -> Bool {
        guard let list = CGWindowListCopyWindowInfo([.optionOnScreenOnly, .excludeDesktopElements], kCGNullWindowID) as? [[String: Any]] else { return false }
        return list.contains { ($0[kCGWindowNumber as String] as? Int) == id }
    }

    /// While the engine is wanted and Wind is zoomed, look for prompts 4 times a second.
    private func startPromptWatch() {
        guard promptTimer == nil else { return }
        promptTimer = Timer.scheduledTimer(withTimeInterval: 0.25, repeats: true) { [weak self] _ in self?.checkPrompts() }
    }

    /// Hand the view to macOS's zoom while something is up the overlay cannot show right, and take it
    /// back after. Also called at once when a screenshot or colour-sampler mode starts or ends
    /// (PointerTakeover): it forces the real cursor visible, which render never magnifies (the small
    /// free crosshair beside Wind's big one), and its selection band stacks above the overlay. Handed
    /// over only once the band appeared (0.25 s into a drag), the view jumped mid-drag (2026-11-01).
    func checkPrompts() {
        let zoomed = visible || DrawZoom.handedOver
        if !zoomed { promptTimer?.invalidate(); promptTimer = nil; return }
        let takeover = PointerTakeover.shared.active
        // Handed over, wait for the window that caused it to close: with the overlay hidden, a
        // window that sat above it (a UserNotificationCenter alert, layer 8) is judged by its
        // level and looked gone, so the engine flipped 4 times a second (2026-11-01).
        if DrawZoom.handedOver, let id = handoverWindow, DrawZoom.windowOnScreen(id) { return }
        let blocking = takeover ? nil : DrawZoom.blockingWindow()
        let prompt = takeover || blocking != nil
        if prompt && !DrawZoom.handedOver {
            Log.info(takeover ? "draw engine: screenshot or colour-sampler mode; macOS zoom shows it"
                              : "draw engine: a window the capture cannot show is up (lock screen, capture-protected or above the overlay); macOS zoom shows it")
            DrawZoom.handedOver = true
            handoverWindow = blocking
            hide()
            DrawZoom.enabled = false
            onHandover?(true)
        } else if !prompt && DrawZoom.handedOver {
            Log.info("draw engine: it is gone; back to drawing")
            DrawZoom.handedOver = false
            handoverWindow = nil
            SkyLight.resetCompositor()                  // leave macOS's zoom before drawing again
            DrawZoom.enabled = true
            onHandover?(false)
        }
    }

    /// Switch the engine off for this run and let macOS's zoom take over.
    private func fail(_ why: String) {
        Log.error("draw engine: \(why); using macOS zoom until Wind restarts")
        DrawZoom.failed = true
        hide()
        stopStream()
        DrawZoom.enabled = false
        onFailure?()
    }

    private func stopStream() {
        generation += 1
        latest = nil
        if let s = stream { stream = nil; s.stopCapture { _ in } }
    }

    /// The current system cursor's image and hotspot, if it changed.
    private func cursorShape() {
        if let cur = NSCursor.currentSystem {
            let img = cur.image
            // Same size and hotspot can still be another cursor (resize, badges): compare pixels.
            let key = img.tiffRepresentation
            if key != cursorKey {
                cursorKey = key; cursorSize = img.size; cursorHotSpot = cur.hotSpot
                var r = CGRect(origin: .zero, size: img.size)
                var cg = img.cgImage(forProposedRect: &r, context: nil, hints: nil)
                #if WIND_TEST_CHANNEL
                // Test probes. WIND_CURSOR_PROBE: a fixed 12 x 12 pt magenta block on the hotspot, so
                // scripts/cursor_truth.py measures where the cursor is, whatever its shape (a silhouette's
                // centroid moved with each shape change and read as jumps). WIND_CURSOR_SHAPE_PROBE: the
                // shape as a magenta silhouette, for scripts/cursor_shape_truth.py (its area).
                let env = ProcessInfo.processInfo.environment
                if env["WIND_CURSOR_PROBE"] != nil && env["WIND_CURSOR_SHAPE_PROBE"] == nil,
                   let ctx = CGContext(data: nil, width: 24, height: 24, bitsPerComponent: 8, bytesPerRow: 0,
                                       space: CGColorSpaceCreateDeviceRGB(), bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue) {
                    ctx.setFillColor(CGColor(red: 1, green: 0, blue: 1, alpha: 1)); ctx.fill(CGRect(x: 0, y: 0, width: 24, height: 24))
                    cg = ctx.makeImage(); cursorSize = CGSize(width: 12, height: 12); cursorHotSpot = CGPoint(x: 6, y: 6)
                }
                if env["WIND_CURSOR_SHAPE_PROBE"] != nil, let src = cg,
                   let ctx = CGContext(data: nil, width: src.width, height: src.height, bitsPerComponent: 8, bytesPerRow: 0,
                                       space: CGColorSpaceCreateDeviceRGB(), bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue) {
                    let rect = CGRect(x: 0, y: 0, width: src.width, height: src.height)
                    ctx.clip(to: rect, mask: src)
                    ctx.setFillColor(CGColor(red: 1, green: 0, blue: 1, alpha: 1)); ctx.fill(rect)
                    cg = ctx.makeImage()
                }
                #endif
                cursorLayer.contents = cg
            }
        }
    }

    /// Called every tick with the pointer (desktop points): place the drawn cursor.
    func updateCursor(_ p: CGPoint) {
        DrawZoom.beat = CACurrentMediaTime()
        guard visible, let screen = NSScreen.screens.first else { return }
        let now = CACurrentMediaTime()
        var shapeDue = false
        // macOS can show the real cursor again (entering the Dock, an app switch): checked every frame
        // (one shared-memory byte), so it shows for a frame at most (it stayed up to 0.1 s, 2026-11-02).
        // Not in the 0.1 s after taking it: the scoreboard can still read shown then, and a forced
        // show+hide right after the order-in blocked the first zoom tick 7 ms on WindowServer (2026-10-31).
        if SkyLight.forceHidden && now - cursorTakenAt > 0.1 { SkyLight.hideRealCursor() }
        if now - lastCursorCheck > 0.1 && SkyLight.forceHidden {   // the shape (arrow, I-beam, ...) changes rarely
            lastCursorCheck = now
            shapeDue = true
        }
        let W = screen.frame.width, H = screen.frame.height
        // Screen position of the pointer (top-left origin), then the layer frame (bottom-left).
        let sx = (p.x - center.x) * scale + W / 2, sy = (p.y - center.y) * scale + H / 2
        lastTickAt = now
        CATransaction.begin(); CATransaction.setDisableActions(true)
        // A new shape goes up in the same commit as its size and place, with no implicit animation:
        // set on its own (outside this transaction) the new image and the new size reached the screen
        // a frame apart, and the cursor showed big for a frame at every shape change (owner
        // 2026-10-31: I-beam, scroll bar, resize edge).
        if shapeDue { cursorShape() }
        let w = cursorSize.width * scale, h = cursorSize.height * scale
        if let p = pendingSurface { image.contents = p; pendingSurface = nil }
        let shooting = shot?.active ?? false
        cursorLayer.isHidden = pointerHidden || lockHidden || shooting   // the selection's crosshair replaces it
        // The selection in the same commit as the cursor: the crosshair at the pointer the view was placed
        // for, the box's anchor where its desktop point is shown now.
        shotLayers.update(shot, map: { CGPoint(x: W / 2 + ($0.x - self.center.x) * self.scale, y: H / 2 - ($0.y - self.center.y) * self.scale) },
                          mag: scale, px: 1)
        crossRoot.isHidden = crosshair == nil           // also on ticks with no view write (at an edge)
        if let c = crosshair { crossRoot.position = CGPoint(x: W / 2 + (c.x - center.x) * scale, y: H / 2 - (c.y - center.y) * scale) }
        cursorLayer.frame = CGRect(x: sx - cursorHotSpot.x * scale, y: H - sy - (h - cursorHotSpot.y * scale), width: w, height: h)
        CATransaction.commit()
    }

    private func layout() {
        guard let screen = NSScreen.screens.first else { return }
        let W = screen.frame.width, H = screen.frame.height
        CATransaction.begin(); CATransaction.setDisableActions(true)
        let filter: CALayerContentsFilter = smooth ? .linear : .nearest
        image.magnificationFilter = filter
        cursorLayer.magnificationFilter = filter
        // The desktop point at `center` sits at the screen centre.
        image.frame = CGRect(x: W / 2 - center.x * scale, y: H / 2 - (H - center.y) * scale, width: W * scale, height: H * scale)
        // The crosshair in the same commit as the view, so both reach the same frame.
        crossRoot.isHidden = crosshair == nil
        if let c = crosshair { crossRoot.position = CGPoint(x: W / 2 + (c.x - center.x) * scale, y: H / 2 - (c.y - center.y) * scale) }
        CATransaction.commit()
    }

    private func makeWindow() {
        guard let screen = NSScreen.screens.first else { return }
        let w = NSWindow(contentRect: screen.frame, styleMask: .borderless, backing: .buffered, defer: false)
        w.level = NSWindow.Level(rawValue: DrawZoom.overlayLevel)
        w.ignoresMouseEvents = true
        w.isOpaque = true
        w.backgroundColor = .black
        w.hasShadow = false
        // No AppKit order-in animation: by default the overlay appeared 1.3-1.7% SMALLER than the
        // screen and grew back over ~120 ms, the "push back" at every zoom-in start (2026-10-31).
        w.animationBehavior = .none
        w.isReleasedWhenClosed = false
        w.collectionBehavior = [.canJoinAllSpaces, .stationary, .fullScreenAuxiliary, .ignoresCycle]
        let v = NSView(frame: CGRect(origin: .zero, size: screen.frame.size))
        v.wantsLayer = true
        v.layer?.backgroundColor = NSColor.black.cgColor
        image.contentsGravity = .resize
        image.minificationFilter = .linear
        v.layer?.addSublayer(image)
        let path = CGMutablePath(), a = 26.0, g = 6.0       // as seen on screen, like Crosshair's
        path.move(to: CGPoint(x: -a, y: 0)); path.addLine(to: CGPoint(x: -g, y: 0))
        path.move(to: CGPoint(x: g, y: 0)); path.addLine(to: CGPoint(x: a, y: 0))
        path.move(to: CGPoint(x: 0, y: -a)); path.addLine(to: CGPoint(x: 0, y: -g))
        path.move(to: CGPoint(x: 0, y: g)); path.addLine(to: CGPoint(x: 0, y: a))
        crossOutline.strokeColor = NSColor.black.withAlphaComponent(0.6).cgColor; crossOutline.lineWidth = 4.4
        crossLine.strokeColor = NSColor.white.cgColor; crossLine.lineWidth = 2
        for l in [crossOutline, crossLine] { l.path = path; l.fillColor = nil; l.lineCap = .butt; crossRoot.addSublayer(l) }
        if Crosshair.probe {                            // test builds: the capture finds it by a magenta block
            crossProbe.backgroundColor = CGColor(srgbRed: 1, green: 0, blue: 1, alpha: 1)
            crossProbe.frame = CGRect(x: -6, y: -6, width: 12, height: 12)
            crossRoot.insertSublayer(crossProbe, at: 0)
        }
        crossRoot.bounds = .zero
        crossRoot.isHidden = true
        v.layer?.addSublayer(crossRoot)
        v.layer?.addSublayer(cursorLayer)
        v.layer?.addSublayer(shotLayers.root)
        w.contentView = v
        SkyLight.ignoreForWindowPicker(windowNumber: w.windowNumber)    // screenshot window mode picked it
        window = w
        if !observing {
            observing = true
            NotificationCenter.default.addObserver(forName: NSApplication.didChangeScreenParametersNotification, object: nil, queue: .main) { [weak self] _ in
                self?.screensChanged()
            }
        }
    }

    /// Resolution, scaling or the main display changed: resize the overlay and recapture.
    private func screensChanged() {
        guard let w = window, let screen = NSScreen.screens.first else { return }
        if NSScreen.screens.count > 1 && visible { fail("a second display was attached (not supported yet)"); return }
        w.setFrame(screen.frame, display: false)
        w.contentView?.frame = CGRect(origin: .zero, size: screen.frame.size)
        stopStream()
        if visible { haveFrame = false; w.alphaValue = 0; startStream() }
        layout()
        Log.info("draw engine: display changed, recapturing")
    }

    private func startStream() {
        guard stream == nil, !starting, let win = window else { return }
        starting = true
        let windowID = CGWindowID(win.windowNumber)
        generation += 1
        let gen = generation
        Task {
            do {
                #if WIND_TEST_CHANNEL
                if ProcessInfo.processInfo.environment["WIND_DRAW_FAIL"] != nil {
                    throw NSError(domain: "Wind", code: 1, userInfo: [NSLocalizedDescriptionKey: "simulated capture failure"])
                }
                #endif
                let content = try await SCShareableContent.excludingDesktopWindows(false, onScreenWindowsOnly: false)
                guard let display = content.displays.first(where: { $0.displayID == CGMainDisplayID() }) else {
                    throw NSError(domain: "Wind", code: 2, userInfo: [NSLocalizedDescriptionKey: "main display not capturable"])
                }
                let own = content.windows.filter { $0.windowID == windowID }
                let filter = SCContentFilter(display: display, excludingWindows: own)
                let cfg = SCStreamConfiguration()
                let px = await MainActor.run { NSScreen.screens.first?.backingScaleFactor ?? 2 }
                cfg.width = Int(Double(display.width) * px); cfg.height = Int(Double(display.height) * px)
                cfg.minimumFrameInterval = CMTime(value: 1, timescale: 240)
                cfg.showsCursor = false
                cfg.queueDepth = 4
                cfg.pixelFormat = kCVPixelFormatType_32BGRA
                let s = SCStream(filter: filter, configuration: cfg, delegate: self)
                try s.addStreamOutput(self, type: .screen, sampleHandlerQueue: DispatchQueue(label: "wind.draw.capture", qos: .userInteractive))
                try await s.startCapture()
                await MainActor.run {
                    self.starting = false
                    if self.generation == gen && (self.visible || self.idleStop != nil) { self.stream = s } else { s.stopCapture { _ in } }
                }
                Log.info("draw engine: capturing \(cfg.width)x\(cfg.height), overlay excluded (\(own.count) window)")
            } catch {
                await MainActor.run {
                    self.starting = false
                    self.fail("capture failed: \(error.localizedDescription) (Screen Recording permission?)")
                }
            }
        }
    }

    func stream(_ s: SCStream, didOutputSampleBuffer sb: CMSampleBuffer, of type: SCStreamOutputType) {
        guard type == .screen,
              let att = CMSampleBufferGetSampleAttachmentsArray(sb, createIfNecessary: false) as? [[SCStreamFrameInfo: Any]],
              let st = att.first?[.status] as? Int, st == SCFrameStatus.complete.rawValue,
              let pb = CMSampleBufferGetImageBuffer(sb), let surf = CVPixelBufferGetIOSurface(pb)?.takeUnretainedValue() else { return }
        DispatchQueue.main.async {
            guard self.stream === s else { return }    // a stopped stream's late frames are dropped
            if self.latest == nil && !self.visible && self.warmingSince > 0 {
                Log.info(String(format: "draw engine: capture warm %.0f ms after the zoom was asked for", (CACurrentMediaTime() - self.warmingSince) * 1000))
                self.warmingSince = 0
            }
            self.latest = surf
            guard self.visible else { return }
            self.frames += 1
            // While the zoom ticks, a captured frame goes up with the tick's own commit: committed
            // here, at a random phase, it was a second commit per refresh racing WindowServer.
            if self.haveFrame && CACurrentMediaTime() - self.lastTickAt < 0.02 {
                self.pendingSurface = surf
                // Only if the link paused: this frame, still pending, and no tick since it arrived.
                // (Committing whatever was pending put a newer frame up off-phase; review 2026-10-31.)
                let arrived = CACurrentMediaTime()
                DispatchQueue.main.asyncAfter(deadline: .now() + 0.012) {
                    guard let p = self.pendingSurface, p === surf, self.lastTickAt < arrived, self.visible else { return }
                    self.pendingSurface = nil
                    CATransaction.begin(); CATransaction.setDisableActions(true); self.image.contents = p; CATransaction.commit()
                }
                return
            }
            self.pendingSurface = nil
            CATransaction.begin(); CATransaction.setDisableActions(true)
            self.image.contents = surf
            CATransaction.commit()
            if !self.haveFrame {
                self.haveFrame = true; self.window?.alphaValue = 1
                self.takeCursor()
                Log.info(String(format: "draw engine: first frame %.0f ms after zooming in", (CACurrentMediaTime() - self.shownAt) * 1000))
            }
        }
    }

    /// The stream ended (display sleep, permission revoked, ...): restart once, else fall back, so a
    /// frozen picture never stays over the live screen.
    func stream(_ s: SCStream, didStopWithError error: Error) {
        DispatchQueue.main.async {
            guard self.stream === s else { return }
            Log.error("draw engine: capture stopped: \(error.localizedDescription)")
            self.stopStream()
            guard self.visible else { return }
            self.restarts += 1
            if self.restarts > 2 { self.fail("capture keeps stopping"); return }
            self.haveFrame = false
            self.window?.alphaValue = 0
            self.startStream()
            let gen = self.generation
            DispatchQueue.main.asyncAfter(deadline: .now() + 1.5) {
                guard self.visible, !self.haveFrame, self.generation == gen else { return }
                self.fail("no captured frame after a restart")
            }
        }
    }

    /// If the main thread stops answering while the overlay hides the real screen, quit: the overlay
    /// and the cursor hiding go with the process, and the user gets the live screen back.
    private var watchdog: DispatchSourceTimer?
    private func startWatchdog() {
        guard watchdog == nil else { return }
        let t = DispatchSource.makeTimerSource(queue: DispatchQueue(label: "wind.draw.watchdog"))
        t.schedule(deadline: .now() + 1, repeating: 0.5)
        t.setEventHandler {
            guard DrawZoom.overlayUp, CACurrentMediaTime() - DrawZoom.beat > 3 else { return }
            fputs("Wind: main thread stuck for 3 s with the draw overlay up; quitting\n", stderr)
            exit(3)
        }
        t.resume()
        watchdog = t
        // The heartbeat also comes from a timer, so an idle (paused) tick does not look like a hang.
        Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { _ in DrawZoom.beat = CACurrentMediaTime() }
    }
}
