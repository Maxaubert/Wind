import AppKit
import QuartzCore

// The real cursor while Wind draws its own (render, and Wind's cursor on macOS zoom): shown, but with a
// transparent image, instead of hidden.
//
// Why not hide it: SwiftUI ignores hover whenever the system says the cursor is hidden. It asks
// +[NSCursor _isHidden] = !SLCursorIsVisible(), which reads WindowServer's shared scoreboard (the hide
// count byte), so one process hiding the cursor turns off SwiftUI hover in every app. Notification
// Center's banners are SwiftUI `onHover`: zoomed, their close button never came (owner 2026-11-02).
// AppKit tracking areas and mouse-moved hover (Terminal) kept working, which is why only some hover broke.
// research/wind-mac/re/2026-11-02-notification-hover-re.md and -repro.md.
//
// How: with SetsCursorInBackground, Wind sets a transparent cursor. Apps set their own cursor whenever
// the shape should change (I-beam over text, resize edges); each set bumps the cursor seed on the
// scoreboard page. A thread reads the seed every 2 ms; on a change that is not Wind's it keeps the app's
// cursor as the shape Wind draws, then sets the transparent one again (measured 2026-11-02 against an app
// that sets a cursor on every mouse move: about 0.2 ms from the app's set to Wind's, and the app's
// cursor in none of ~580 captured frames). When Wind exits or crashes WindowServer drops its cursor and
// shows the app's again; if Wind's main thread stalls, the thread stops re-setting it and puts the app's
// cursor back, so a stuck Wind never leaves the pointer invisible.
final class ClearCursor: @unchecked Sendable {
    static let shared = ClearCursor()

    private let lock = NSLock()
    private var active = false
    private var session = 0                 // a thread runs while its session is the current one
    private var appCursor: NSCursor?        // what apps set last: the shape Wind draws
    private var shapeGen = 0
    private var ours: UInt32 = 0            // the seed of Wind's own transparent set
    private var beat = 0.0                  // main thread alive (a timer while active)
    private var stalled = false
    private var beatTimer: Timer?
    /// Wind's transparent cursor. Its size and hotspot are its signature: a read of the system cursor
    /// that shows them is Wind's own set, not an app's.
    private static let clearSize = NSSize(width: 7, height: 5)
    private static let clearHot = NSPoint(x: 3, y: 2)
    private let clear = NSCursor(image: NSImage(size: ClearCursor.clearSize, flipped: false) { _ in true }, hotSpot: ClearCursor.clearHot)

    /// True while the real cursor is shown transparent.
    var isActive: Bool { lock.lock(); defer { lock.unlock() }; return active }

    /// The cursor Wind should draw and a counter that changes with it. While active it is the app's
    /// cursor kept from before Wind's transparent set; otherwise the system cursor.
    func shape() -> (cursor: NSCursor?, gen: Int) {
        lock.lock()
        if active { let r = (appCursor, shapeGen); lock.unlock(); return r }
        lock.unlock()
        return (NSCursor.currentSystem, -1)
    }

    /// The shape counter alone (cheap; -1 while inactive).
    var generation: Int { lock.lock(); defer { lock.unlock() }; return active ? shapeGen : -1 }

    /// Start (or keep) the transparent real cursor. Main thread; cheap when already active.
    func start() {
        lock.lock()
        if active { beat = CACurrentMediaTime(); lock.unlock(); return }
        lock.unlock()
        SkyLight.allowBackgroundCursor()
        let first = NSCursor.currentSystem
        lock.lock()
        active = true; stalled = false; session += 1
        if let c = first, !isClear(c) { appCursor = c; shapeGen += 1 }
        beat = CACurrentMediaTime()
        let s = session
        lock.unlock()
        setClear()
        let t = Thread { [weak self] in self?.poll(session: s) }
        t.name = "wind.clearcursor"; t.qualityOfService = .userInteractive
        t.start()
        beatTimer?.invalidate()
        beatTimer = Timer.scheduledTimer(withTimeInterval: 0.25, repeats: true) { [weak self] _ in
            guard let self else { return }
            self.lock.lock(); self.beat = CACurrentMediaTime(); self.lock.unlock()
        }
    }

    /// Back to the app's cursor. Main thread.
    func stop() {
        lock.lock()
        guard active else { lock.unlock(); return }
        active = false; session += 1
        let app = appCursor
        lock.unlock()
        beatTimer?.invalidate(); beatTimer = nil
        (app ?? NSCursor.arrow).set()
    }

    private func isClear(_ c: NSCursor) -> Bool {
        c.image.size == ClearCursor.clearSize && c.hotSpot == ClearCursor.clearHot
    }

    private func setClear() {
        clear.set()
        let s = SkyLight.cursorSeed
        lock.lock(); ours = s; lock.unlock()
    }

    private func poll(session s: Int) {
        var last = SkyLight.cursorSeed
        while true {
            usleep(2000)
            lock.lock()
            guard active, session == s else { lock.unlock(); return }
            let stale = CACurrentMediaTime() - beat > 1.5
            let wasStalled = stalled, mine = ours, app = appCursor
            if stale != stalled { stalled = stale }
            lock.unlock()
            if stale {
                // Wind's main thread is stuck: show the app's cursor until it is back.
                if !wasStalled { (app ?? NSCursor.arrow).set() }
                continue
            }
            let seed = SkyLight.cursorSeed
            if seed == last && !wasStalled { continue }
            last = seed
            if seed == mine && !wasStalled { continue }
            // Someone else set the cursor (or Wind's own set raced with it): keep theirs as the shape.
            if let c = NSCursor.currentSystem, !isClear(c) {
                lock.lock()
                if active && session == s { appCursor = c; shapeGen += 1 }
                lock.unlock()
            } else if !wasStalled {
                lock.lock(); ours = seed; lock.unlock()   // it is Wind's own transparent cursor
                continue
            }
            lock.lock(); let still = active && session == s; lock.unlock()
            guard still else { return }
            setClear()
            last = SkyLight.cursorSeed
        }
    }
}
