import AppKit

// macOS modes that take the pointer over: screenshot selection (Cmd+Shift+4 and its window/camera mode,
// Cmd+Shift+5, `screencapture -i`) and the colour sampler loupe (NSColorSampler). They capture all
// events, track the pointer in desktop points, draw their own cursor (screencapture forces the real
// one visible; Wind's re-hide does not win) and read the screen under the pointer. Wind's drawn cursor
// then showed twice (owner 2026-11-01: "one big crosshair in the centre and a small free one" on
// render; a doubled, lagging label on macOS's zoom), went into the saved shot, was what window mode and
// the sampler picked, and render showed the selection band unzoomed. macOS's own zoom has no special
// case: it magnifies every window and the real cursor. So while one is up Wind draws no cursor and
// render hands the view to macOS's zoom, like a system prompt (research/wind-mac/re/2026-11-01-crosshair-*.md).
//
// Detection, polled 10 times a second while zoomed, off the main thread: SLSAreEventsCaptured waits on
// WindowServer (median 46 us, but p99 6 ms and up to 11 ms, 2026-11-01), enough to make a zoom tick
// late. The window list is read only while events are captured.
// - `com.apple.screencapture.interactive` is checked in with launchd only while an interactive
//   capture runs (screencapture uses it to refuse a second one);
// - SLSAreEventsCaptured is true for screencapture and the sampler; menus may set it too, so it counts
//   only with a window of screencapture or ColorSampler on screen.

final class PointerTakeover {
    static let shared = PointerTakeover()
    /// Main thread: a mode that takes the pointer over is up (as of the last poll).
    private(set) var active = false
    /// Called on the main thread when `active` changes.
    var onChange: ((Bool) -> Void)?
    private let q = DispatchQueue(label: "wind.takeover", qos: .userInitiated)
    private var timer: DispatchSourceTimer?
    private var generation = 0                     // results of a stopped watch are dropped
    private var suppressUntil = 0.0                // a session Wind just ended still shows for a poll or two

    /// Main thread. Poll while Wind is zoomed; stopping clears the state (at 1x the real cursor is
    /// macOS's anyway).
    func watch(_ on: Bool) {
        if on {
            guard timer == nil else { return }
            generation += 1
            let gen = generation
            let t = DispatchSource.makeTimerSource(queue: q)
            t.schedule(deadline: .now(), repeating: 0.1, leeway: .milliseconds(10))
            t.setEventHandler { [weak self] in
                let now = PointerTakeover.detect()
                DispatchQueue.main.async { self?.update(now, gen) }
            }
            t.resume()
            timer = t
        } else {
            timer?.cancel(); timer = nil
            generation += 1
            if active { active = false; Log.info("screenshot or colour-sampler mode: zoom off"); onChange?(false) }
        }
    }

    /// Main thread: Wind ended macOS's session itself (its own selection takes over): drop the mode now
    /// and ignore it while screencapture is still going away.
    func suppress() {
        suppressUntil = CACurrentMediaTime() + 1.5
        if active { active = false; onChange?(false) }
    }

    private func update(_ detected: Bool, _ gen: Int) {
        let now = detected && CACurrentMediaTime() >= suppressUntil
        guard gen == generation, now != active else { return }
        active = now
        Log.info(now ? "screenshot or colour-sampler mode: Wind's cursor off, macOS's zoom shows it" : "screenshot or colour-sampler mode ended")
        onChange?(now)
    }

    private typealias LookUpFn = @convention(c) (mach_port_t, UnsafePointer<CChar>, UnsafeMutablePointer<mach_port_t>) -> kern_return_t
    private typealias ConnFn = @convention(c) () -> Int32
    private typealias CapturedFn = @convention(c) (Int32, UnsafeMutablePointer<Bool>) -> Int32
    private static let rtDefault = UnsafeMutableRawPointer(bitPattern: -2)   // RTLD_DEFAULT
    private static let lookUp = dlsym(rtDefault, "bootstrap_look_up").map { unsafeBitCast($0, to: LookUpFn.self) }
    private static let bootstrapPort = dlsym(rtDefault, "bootstrap_port")
    private static let eventsCaptured: CapturedFn? = {
        guard let h = dlopen("/System/Library/PrivateFrameworks/SkyLight.framework/SkyLight", RTLD_LAZY),
              let f = dlsym(h, "SLSAreEventsCaptured") else { return nil }
        return unsafeBitCast(f, to: CapturedFn.self)
    }()
    private static let owners: Set<String> = ["screencapture", "ColorSampler"]

    static func detect() -> Bool {
        #if WIND_TEST_CHANNEL
        if ProcessInfo.processInfo.environment["WIND_NO_TAKEOVER"] != nil { return false }   // tests: the old behaviour
        #endif
        if let f = lookUp, let bp = bootstrapPort {
            var port: mach_port_t = 0
            if f(bp.load(as: mach_port_t.self), "com.apple.screencapture.interactive", &port) == KERN_SUCCESS {
                mach_port_deallocate(mach_task_self_, port)
                return true
            }
        }
        var captured = false
        guard let f = eventsCaptured, f(SkyLight.cid, &captured) == 0, captured,
              let list = CGWindowListCopyWindowInfo([.optionOnScreenOnly, .excludeDesktopElements], kCGNullWindowID) as? [[String: Any]] else { return false }
        return list.contains { owners.contains($0[kCGWindowOwnerName as String] as? String ?? "") }
    }
}
