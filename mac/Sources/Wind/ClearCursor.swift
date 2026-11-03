import AppKit
import ObjectiveC
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
    private var resetCount = 0              // transparent sets after another process's set (tests)
    private var foreignCount = 0
    private var ownCount = 0                // Wind's own windows' cursor sets turned transparent at the source (tests)
    private var beatTimer: Timer?
    /// Wind's transparent cursor. Its size and hotspot are its signature: a read of the system cursor
    /// that shows them is Wind's own set, not an app's.
    private static let clearSize = NSSize(width: 7, height: 5)
    private static let clearHot = NSPoint(x: 3, y: 2)
    private let clear = NSCursor(image: NSImage(size: ClearCursor.clearSize, flipped: false) { _ in true }, hotSpot: ClearCursor.clearHot)

    /// Seed changes seen and transparent re-sets so far (tests).
    var counts: [Int] { lock.lock(); defer { lock.unlock() }; return [foreignCount, resetCount, ownCount] }

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
        SkyLight.setOwnsCursor(true)                    // over the Dock Wind's sets are refused otherwise
        ClearCursor.hookOwnSets()
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
        // In common modes: a default-mode timer stops while a menu tracks, and with the pointer still in
        // Wind's own menu-bar menu the poll thread took that for a stall and showed the app's cursor, tiny
        // and unmagnified, after 1.5 s (2026-11-03).
        let bt = Timer(timeInterval: 0.25, repeats: true) { [weak self] _ in
            guard let self else { return }
            self.lock.lock(); self.beat = CACurrentMediaTime(); self.lock.unlock()
        }
        RunLoop.main.add(bt, forMode: .common)
        beatTimer = bt
    }

    /// Back to the app's cursor. Main thread.
    func stop() {
        lock.lock()
        guard active else { lock.unlock(); return }
        active = false; session += 1
        let app = appCursor
        lock.unlock()
        beatTimer?.invalidate(); beatTimer = nil
        (app ?? NSCursor.arrow).set()                   // while Wind still owns the cursor (over the Dock)
        SkyLight.updateOwnsCursor()                     // then give it back, unless Wind still hides it
    }

    /// The system cursor reads as Wind's transparent one (what the owner sees: tests).
    var systemIsClear: Bool { NSCursor.currentSystem.map(isClear) ?? false }

    private func isClear(_ c: NSCursor) -> Bool {
        c.image.size == ClearCursor.clearSize && c.hotSpot == ClearCursor.clearHot
    }

    /// Set the transparent cursor. From the poll thread (`session`) only while that session is still
    /// the active one, checked under the lock held across the set: otherwise stop() could put the app's
    /// cursor back between the check and the set, leaving the real cursor transparent at 1x.
    @discardableResult
    private func setClear(session s: Int? = nil) -> Bool {
        lock.lock(); defer { lock.unlock() }
        if let s, !(active && session == s) { return false }
        clear.set()
        let seed = SkyLight.cursorSeed
        // An app's set can land between Wind's set and the seed read: then that seed is the app's, and
        // taking it as Wind's would leave the app's cursor showing unzoomed until its next set. 0 never
        // matches, so the poll handles it as an app's set.
        ours = NSCursor.currentSystem.map(isClear) == true ? seed : 0
        return true
    }

    // Wind's own windows (Settings, a WKWebView, foreground while open) set the cursor in THIS process:
    // WebKit on every hover change (the pointing hand over links and controls), AppKit's cursor rects.
    // Caught only by the seed poll, each set showed the real, unmagnified cursor until the poll's re-set
    // (up to 2 ms), so it reached the screen in a frame now and then: over the zoomed Settings window
    // the real cursor showed in 5% of 5 ms samples, about 20 glimpses a second (owner 2026-11-03: "in
    // settings the tiny cursor reappears"). Here such a set is taken at the source while active: its
    // cursor becomes the shape Wind draws and the transparent one is set instead, so it never shows.
    private typealias SetIMP = @convention(c) (NSCursor, Selector) -> Void
    nonisolated(unsafe) private static var originalSet: SetIMP?
    private static let setSel = #selector(NSCursor.set)

    /// Replace -[NSCursor set] in Wind's process once (main thread).
    private static func hookOwnSets() {
        guard originalSet == nil, let m = class_getInstanceMethod(NSCursor.self, setSel) else { return }
        let block: @convention(block) (NSCursor) -> Void = { c in ClearCursor.shared.ownSet(c) }
        originalSet = unsafeBitCast(method_setImplementation(m, imp_implementationWithBlock(block)), to: SetIMP.self)
    }

    private func ownSet(_ c: NSCursor) {
        guard let orig = ClearCursor.originalSet else { return }
        if c === clear { orig(c, ClearCursor.setSel); return }   // Wind's own transparent set (setClear, under the lock)
        lock.lock()
        // Passed through when not active (stop() puts the app's cursor back after clearing `active`) or
        // while the main thread is stalled (the poll thread puts the app's cursor back).
        guard active && !stalled else { lock.unlock(); orig(c, ClearCursor.setSel); return }
        appCursor = c; shapeGen += 1; ownCount += 1
        orig(clear, ClearCursor.setSel)
        // Not taken as Wind's seed without a read of the system cursor (a WindowServer round trip, too
        // slow for every hover change on the main thread): 0 lets the poll check it off the main thread.
        ours = 0
        lock.unlock()
    }

    private func poll(session s: Int) {
        lock.lock(); var last = ours; lock.unlock()     // 0 (start's set raced with an app's): look at once
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
                if !wasStalled { (app ?? NSCursor.arrow).set(); Log.info("clear cursor: main thread quiet 1.5 s, the app's cursor shown") }
                continue
            }
            let seed = SkyLight.cursorSeed
            if seed == last && !wasStalled { continue }
            last = seed
            if seed == mine && !wasStalled { continue }
            // Someone else set the cursor (or Wind's own set raced with it): keep theirs as the shape.
            lock.lock(); foreignCount += 1; lock.unlock()
            if let c = NSCursor.currentSystem, !isClear(c) {
                lock.lock()
                if active && session == s { appCursor = c; shapeGen += 1 }
                lock.unlock()
            } else if !wasStalled {
                lock.lock(); ours = seed; lock.unlock()   // it is Wind's own transparent cursor
                continue
            }
            guard setClear(session: s) else { return }
            lock.lock(); resetCount += 1; last = ours; lock.unlock()
        }
    }
}
