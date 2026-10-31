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

    private var window: NSWindow?
    private let image = CALayer()
    private let cursorLayer = CALayer()
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
            if let l = latest { CATransaction.begin(); CATransaction.setDisableActions(true); image.contents = l; CATransaction.commit() }
            window?.alphaValue = haveFrame ? 1 : 0      // else shown with the first captured frame
            window?.orderFrontRegardless()
            shownAt = CACurrentMediaTime()
            pointerHidden = SkyLight.wantHidden         // carry Wind's own hide state over
            SkyLight.forceHidden = true
            SkyLight.hideRealCursor()
            if stream == nil { startStream() }
            startPromptWatch()
            let gen = generation
            DispatchQueue.main.asyncAfter(deadline: .now() + 1.5) { [weak self] in
                guard let self, self.visible, !self.haveFrame, self.generation == gen else { return }
                self.fail("no captured frame within 1.5 s")
            }
        }
        layout()
    }

    func hide() {
        guard visible else { return }
        visible = false
        DrawZoom.overlayUp = false
        window?.orderOut(nil)
        SkyLight.forceHidden = false
        SkyLight.restoreWanted()                        // the real cursor as Wind wants it at 1x
        // Keep capturing for a minute so the next zoom-in shows at once; then stop (power, and the
        // screen-recording indicator).
        let stop = DispatchWorkItem { [weak self] in
            guard let self, !self.visible else { return }
            self.stopStream()
            Log.info("draw engine: capture stopped after a minute at 1x")
        }
        idleStop = stop
        DispatchQueue.main.asyncAfter(deadline: .now() + 60, execute: stop)
        Log.info("draw engine: off after \(frames) frames")
        frames = 0
    }

    /// The overlay's window level: above every app and almost every prompt; below the lock-screen
    /// shield. (Password and Touch ID prompts, SecurityAgent and coreautha, are stacked above any
    /// app window whatever its level: measured 2026-10-31 at the maximum level and with every
    /// SkyLight tag bit. Only macOS's zoom can magnify them.)
    static let overlayLevel = Int(CGShieldingWindowLevel()) - 1

    /// The lock screen and screen saver: not captured, so macOS's zoom shows them.
    private static let lockOwners: Set<String> = ["loginwindow", "ScreenSaverEngine"]

    /// Whether something is on screen the drawn zoom cannot show right: a window stacked above the
    /// overlay (it would show twice, unzoomed on top of its magnified copy), a window that opts out
    /// of screen capture (it would be hidden), or the lock screen. Every other window, prompts
    /// included, is captured and drawn magnified, and clicks land on it where it is drawn (the
    /// Screen Recording prompt, universalAccessAuthWarn, sits under the overlay: it stays drawn).
    static func promptOnScreen() -> Bool {
        guard let list = CGWindowListCopyWindowInfo([.optionOnScreenOnly, .excludeDesktopElements], kCGNullWindowID) as? [[String: Any]] else { return false }
        let me = ProcessInfo.processInfo.processIdentifier
        var ignore = Set<String>()
        #if WIND_TEST_CHANNEL
        // Tests can ignore a real window left on screen (owner away).
        if let v = ProcessInfo.processInfo.environment["WIND_IGNORE_PROMPTS"] { ignore = Set(v.split(separator: ",").map(String.init)) }
        #endif
        var aboveOverlay = true                         // the list runs front to back
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
            if lockOwners.contains(owner) || sharing == 0 { return true }
            // Above the overlay. Before the overlay is first shown, judge by level: prompts that
            // macOS stacks over everything are the ones at or above the screen-saver level.
            if aboveOverlay && layer >= NSWindow.Level.screenSaver.rawValue { return true }
        }
        return false
    }

    /// While the engine is wanted and Wind is zoomed, look for prompts 4 times a second.
    private func startPromptWatch() {
        guard promptTimer == nil else { return }
        promptTimer = Timer.scheduledTimer(withTimeInterval: 0.25, repeats: true) { [weak self] _ in
            guard let self else { return }
            let zoomed = self.visible || DrawZoom.handedOver
            if !zoomed { self.promptTimer?.invalidate(); self.promptTimer = nil; return }
            let prompt = DrawZoom.promptOnScreen()
            if prompt && !DrawZoom.handedOver {
                Log.info("draw engine: a window the capture cannot show is up (lock screen or capture-protected); macOS zoom shows it")
                DrawZoom.handedOver = true
                self.hide()
                DrawZoom.enabled = false
                self.onHandover?(true)
            } else if !prompt && DrawZoom.handedOver {
                Log.info("draw engine: it is gone; back to drawing")
                DrawZoom.handedOver = false
                SkyLight.resetCompositor()              // leave macOS's zoom before drawing again
                DrawZoom.enabled = true
                self.onHandover?(false)
            }
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

    /// Called every tick with the pointer (desktop points): place the drawn cursor.
    func updateCursor(_ p: CGPoint) {
        DrawZoom.beat = CACurrentMediaTime()
        guard visible, let screen = NSScreen.screens.first else { return }
        let now = CACurrentMediaTime()
        if now - lastCursorCheck > 0.1 {                // the shape (arrow, I-beam, ...) changes rarely
            lastCursorCheck = now
            SkyLight.hideRealCursor()                   // macOS can show it again (Dock, app switch)
            if let cur = NSCursor.currentSystem {
                let img = cur.image
                // Same size and hotspot can still be another cursor (resize, badges): compare pixels.
                let key = img.tiffRepresentation
                if key != cursorKey {
                    cursorKey = key; cursorSize = img.size; cursorHotSpot = cur.hotSpot
                    var r = CGRect(origin: .zero, size: img.size)
                    cursorLayer.contents = img.cgImage(forProposedRect: &r, context: nil, hints: nil)
                }
            }
        }
        let W = screen.frame.width, H = screen.frame.height
        // Screen position of the pointer (top-left origin), then the layer frame (bottom-left).
        let sx = (p.x - center.x) * scale + W / 2, sy = (p.y - center.y) * scale + H / 2
        let w = cursorSize.width * scale, h = cursorSize.height * scale
        CATransaction.begin(); CATransaction.setDisableActions(true)
        cursorLayer.isHidden = pointerHidden || lockHidden
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
        w.isReleasedWhenClosed = false
        w.collectionBehavior = [.canJoinAllSpaces, .stationary, .fullScreenAuxiliary, .ignoresCycle]
        let v = NSView(frame: CGRect(origin: .zero, size: screen.frame.size))
        v.wantsLayer = true
        v.layer?.backgroundColor = NSColor.black.cgColor
        image.contentsGravity = .resize
        image.minificationFilter = .linear
        v.layer?.addSublayer(image)
        v.layer?.addSublayer(cursorLayer)
        w.contentView = v
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
            self.latest = surf
            guard self.visible else { return }
            CATransaction.begin(); CATransaction.setDisableActions(true)
            self.image.contents = surf
            CATransaction.commit()
            self.frames += 1
            if !self.haveFrame {
                self.haveFrame = true; self.window?.alphaValue = 1
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
            DispatchQueue.main.asyncAfter(deadline: .now() + 1.5) { [weak self] in
                guard let self, self.visible, !self.haveFrame, self.generation == gen else { return }
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
