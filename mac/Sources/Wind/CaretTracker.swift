import AppKit
import ApplicationServices
import WindCore

// Follows the text caret and keyboard focus of the frontmost app through the Accessibility API
// (Wind's focus_track, macOS edition). Publishes a TrackSnapshot with a new seq whenever the
// caret or focused element moves; the view-owner rules decide whether the view follows.

final class CaretTracker {
    private(set) var snapshot = TrackSnapshot()
    private var observer: AXObserver?
    private var appElement: AXUIElement?
    private var pid: pid_t = 0
    private var isTerminal = false

    /// Terminal emulators: TUIs running in them (Claude Code, editors) park the hidden cursor at the
    /// window's right edge while they redraw, for example on every space in Claude Code's prompt.
    static let chromiumBundles: Set<String> = [
        "com.google.Chrome", "com.google.Chrome.beta", "com.google.Chrome.canary", "org.chromium.Chromium",
        "com.brave.Browser", "com.microsoft.edgemac", "company.thebrowser.Browser", "com.vivaldi.Vivaldi",
        "com.operasoftware.Opera",
    ]

    static let terminalBundles: Set<String> = [
        "com.apple.Terminal", "com.googlecode.iterm2", "com.mitchellh.ghostty", "com.github.wez.wezterm",
        "org.alacritty", "net.kovidgoyal.kitty", "dev.warp.Warp-Stable", "co.zeit.hyper",
    ]
    private var seq: UInt = 0
    var enabled = true { didSet { if enabled != oldValue { attachFrontmost() } } }
    /// Called on the main thread when a new caret or focus is published (wakes the magnifier).
    var onChange: (() -> Void)?

    init() {
        NSWorkspace.shared.notificationCenter.addObserver(
            forName: NSWorkspace.didActivateApplicationNotification, object: nil, queue: .main
        ) { [weak self] _ in self?.attachFrontmost() }
        attachFrontmost()
    }

    func attachFrontmost() {
        detach()
        guard enabled, let app = NSWorkspace.shared.frontmostApplication,
              app.processIdentifier != getpid() else { return }
        pid = app.processIdentifier
        reEnhanced = false
        isTerminal = CaretTracker.terminalBundles.contains(app.bundleIdentifier ?? "")
        let el = AXUIElementCreateApplication(pid)
        // A hung app must not stall the main thread, which also runs the event tap.
        AXUIElementSetMessagingTimeout(el, 0.25)
        appElement = el
        // Electron apps build their accessibility tree only when asked; Chromium browsers need
        // AXEnhancedUserInterface (measured: Chrome sent no caret after a zoom-in until a click).
        AXUIElementSetAttributeValue(el, "AXManualAccessibility" as CFString, kCFBooleanTrue)
        isChromium = CaretTracker.chromiumBundles.contains(app.bundleIdentifier ?? "")
        applyEnhanced()
        if isChromium {
            // A browser still launching can ignore the switch: set it again a second later.
            DispatchQueue.main.asyncAfter(deadline: .now() + 1.0) { [weak self] in
                guard let self, self.pid == pid, let el = self.appElement else { return }
                AXUIElementSetAttributeValue(el, "AXManualAccessibility" as CFString, kCFBooleanTrue)
                if self.enhancedOn != nil { AXUIElementSetAttributeValue(el, "AXEnhancedUserInterface" as CFString, kCFBooleanTrue) }
            }
        }
        var obs: AXObserver?
        let cb: AXObserverCallback = { _, element, notification, refcon in
            guard let refcon else { return }
            let me = Unmanaged<CaretTracker>.fromOpaque(refcon).takeUnretainedValue()
            me.handle(notification as String, element: element)
        }
        guard AXObserverCreate(pid, cb, &obs) == .success, let obs else {
            Log.verbose("caret: no observer for pid \(pid)")
            return
        }
        observer = obs
        let ref = Unmanaged.passUnretained(self).toOpaque()
        for n in [kAXFocusedUIElementChangedNotification, kAXSelectedTextChangedNotification, kAXValueChangedNotification] {
            AXObserverAddNotification(obs, el, n as CFString, ref)
        }
        CFRunLoopAddSource(CFRunLoopGetMain(), AXObserverGetRunLoopSource(obs), .defaultMode)
        Log.verbose("caret: attached to \(app.localizedName ?? "?") (\(pid))")
    }

    /// Chromium's full accessibility mode slows window managers and costs CPU, so it is on only
    /// while a Chromium browser is the frontmost app and switched back off when it loses focus or
    /// Wind quits. (Turning it on only at zoom-in was too late: Chrome builds its tree first and
    /// the first keys after a zoom-in were missed, measured 2026-10-30.)
    var zoomed = false
    private var isChromium = false
    private var enhancedOn: AXUIElement?

    private func applyEnhanced() {
        let want = enabled && isChromium && appElement != nil
        if want, enhancedOn == nil, let el = appElement {
            AXUIElementSetAttributeValue(el, "AXEnhancedUserInterface" as CFString, kCFBooleanTrue)
            enhancedOn = el
        } else if !want, let el = enhancedOn {
            AXUIElementSetAttributeValue(el, "AXEnhancedUserInterface" as CFString, kCFBooleanFalse)
            enhancedOn = nil
        }
    }

    /// Wind is quitting: leave no app in a mode Wind turned on.
    func stop() { detach() }

    private func detach() {
        if let el = enhancedOn {
            AXUIElementSetAttributeValue(el, "AXEnhancedUserInterface" as CFString, kCFBooleanFalse)
            enhancedOn = nil
        }
        if let obs = observer {
            CFRunLoopRemoveSource(CFRunLoopGetMain(), AXObserverGetRunLoopSource(obs), .defaultMode)
        }
        observer = nil
        appElement = nil
        settleTimer?.invalidate(); settleTimer = nil
        pending = nil; pendingSince = nil
    }

    private func handle(_ notification: String, element: AXUIElement) {
        let el = focusedElement() ?? element
        if notification == kAXFocusedUIElementChangedNotification as String {
            if let c = caretInfo(of: el) {
                if !parkedInTerminal(c, el) { publish(.caret, c.rect) }
            } else if let r = frame(of: el) {
                publish(.focus, r)
            }
        } else if let c = caretInfo(of: el), !parkedInTerminal(c, el) {
            publish(.caret, c.rect)
        }
    }

    /// A terminal caret in the last two columns of the text area is a TUI's parked cursor, not
    /// where the user types (measured in Claude Code: every space parked it at the right edge and
    /// the next letter brought it back, so the view jumped away and back).
    private func parkedInTerminal(_ c: (rect: Rc, insertion: Bool), _ el: AXUIElement) -> Bool {
        // Only an insertion point can be parked; a selection reaching the edge is real.
        guard isTerminal, c.insertion, let f = frame(of: el, anySize: true) else { return false }
        let r = c.rect
        let cell = max(4, r.height * 0.5)                 // a terminal cell is about half as wide as tall
        let parked = r.r >= f.r - 2.5 * cell
        if parked { Log.verbose("caret: ignored parked terminal cursor at \(Int(r.l)),\(Int(r.t))") }
        return parked
    }

    /// A caret must hold still this long before it is published. Terminal apps (and TUIs inside
    /// them) move the cursor around in bursts while redrawing; only where it settles counts.
    static let settleMs = 60.0
    /// A caret that never holds still (a busy TUI) is still published after this long.
    static let maxWaitMs = 250.0
    private var pending: (TrackKind, Rc)?
    private var pendingSince: Date?
    private var settleTimer: Timer?

    /// Ask the focused element for its caret shortly after a key press. Notifications alone are not
    /// enough: some apps send none for the first keys after focusing a field (owner report: Notes
    /// and Claude Code sometimes did not track until the zoom moved), and some send none at all.
    /// Throttled, not debounced: a key repeat faster than the delay still polls. After an app
    /// fails to answer (hung), polling pauses for a second so the main thread (which also runs
    /// the event tap) is not held up key after key.
    func pollSoon() {
        guard enabled, pollTimer?.isValid != true, Date() >= pollPausedUntil else { return }
        if observer == nil { attachFrontmost() }
        pollTimer = Timer.scheduledTimer(withTimeInterval: 0.04, repeats: false) { [weak self] _ in
            guard let self else { return }
            guard let el = self.focusedElement() else {
                // A Chromium browser launched just before Wind attached took the accessibility switch
                // too early and reports no focused element (Tab moved the focus, the view stayed put;
                // e2e_focus, 2026-11-01). Switch it on again and look once more shortly after.
                if self.isChromium, !self.reEnhanced, let app = self.appElement {
                    self.reEnhanced = true
                    AXUIElementSetAttributeValue(app, "AXManualAccessibility" as CFString, kCFBooleanTrue)
                    AXUIElementSetAttributeValue(app, "AXEnhancedUserInterface" as CFString, kCFBooleanTrue)
                    self.enhancedOn = app
                    Log.verbose("caret: Chromium had no focused element, accessibility switched on again")
                    // and look again once its tree is up, for this same key
                    DispatchQueue.main.asyncAfter(deadline: .now() + 0.3) { [weak self] in self?.pollSoon() }
                }
                return
            }
            let started = Date()
            if let c = self.caretInfo(of: el) {
                if !self.parkedInTerminal(c, el) { self.publish(.caret, c.rect) }
            } else if let r = self.frame(of: el) {
                // A focused control without a caret (button, link, list): keyboard focus. Chromium
                // freshly launched sent no focus notifications, so Tab moved the focus and the view
                // stayed put (e2e_focus, 2026-11-01); the poll after the key finds it. An unchanged
                // focus is not published again (commit), so other keys do not take the view back.
                self.publish(.focus, r)
            }
            if Date().timeIntervalSince(started) > 0.2 { self.pollPausedUntil = Date().addingTimeInterval(1) }
        }
    }
    private var pollPausedUntil = Date.distantPast
    private var reEnhanced = false              // the Chromium switch was re-applied once for this app
    private var pollTimer: Timer?

    private func publish(_ kind: TrackKind, _ r: Rc) {
        if let p = pending, p.0 == kind, p.1 == r { return }    // a repeat does not restart the wait
        Log.verbose("caret raw: \(kind) \(Int(r.l)),\(Int(r.t)) \(Int(r.width))x\(Int(r.height))")
        pending = (kind, r)
        let since = pendingSince ?? Date()
        pendingSince = since
        let waited = Date().timeIntervalSince(since) * 1000
        let delay = max(0, min(CaretTracker.settleMs, CaretTracker.maxWaitMs - waited)) / 1000
        settleTimer?.invalidate()
        settleTimer = Timer.scheduledTimer(withTimeInterval: delay, repeats: false) { [weak self] _ in
            self?.commit()
        }
    }

    private func commit() {
        guard let (kind, r) = pending else { return }
        pending = nil
        pendingSince = nil
        if snapshot.kind == kind && snapshot.rect == r { return }
        seq &+= 1
        snapshot = TrackSnapshot(kind: kind, seq: seq, rect: r)
        onChange?()
        Log.verbose("caret: \(kind) \(Int(r.l)),\(Int(r.t)) \(Int(r.width))x\(Int(r.height))")
    }

    private func focusedElement() -> AXUIElement? {
        guard let app = appElement else { return nil }
        var v: CFTypeRef?
        guard AXUIElementCopyAttributeValue(app, kAXFocusedUIElementAttribute as CFString, &v) == .success,
              let v, CFGetTypeID(v) == AXUIElementGetTypeID() else { return nil }
        return (v as! AXUIElement)
    }

    /// Bounds of the insertion point (or selection) in global top-left points.
    /// The insertion point (or selection) and whether it is a plain insertion point.
    private func caretRect(of el: AXUIElement) -> Rc? { caretInfo(of: el)?.rect }

    private func caretInfo(of el: AXUIElement) -> (rect: Rc, insertion: Bool)? {
        var rangeVal: CFTypeRef?
        guard AXUIElementCopyAttributeValue(el, kAXSelectedTextRangeAttribute as CFString, &rangeVal) == .success,
              let rangeVal, CFGetTypeID(rangeVal) == AXValueGetTypeID() else { return nil }
        var range = CFRange()
        guard AXValueGetValue(rangeVal as! AXValue, .cfRange, &range) else { return nil }
        if let r = bounds(el, range), r.height > 0 { return (r, range.length == 0) }
        // An empty range often has no bounds: measure the character before the caret instead.
        if range.length == 0 && range.location > 0,
           let r = bounds(el, CFRange(location: range.location - 1, length: 1)), r.height > 0 {
            return (Rc(l: r.r, t: r.t, r: r.r + 1, b: r.b), true)
        }
        return nil
    }

    private func bounds(_ el: AXUIElement, _ range: CFRange) -> Rc? {
        var range = range
        guard let rv = AXValueCreate(.cfRange, &range) else { return nil }
        var out: CFTypeRef?
        guard AXUIElementCopyParameterizedAttributeValue(el, kAXBoundsForRangeParameterizedAttribute as CFString, rv, &out) == .success,
              let out, CFGetTypeID(out) == AXValueGetTypeID() else { return nil }
        var rect = CGRect.zero
        guard AXValueGetValue(out as! AXValue, .cgRect, &rect) else { return nil }
        return Rc(l: rect.minX, t: rect.minY, r: rect.maxX, b: rect.maxY)
    }

    private func frame(of el: AXUIElement, anySize: Bool = false) -> Rc? {
        var pv: CFTypeRef?, sv: CFTypeRef?
        guard AXUIElementCopyAttributeValue(el, kAXPositionAttribute as CFString, &pv) == .success,
              AXUIElementCopyAttributeValue(el, kAXSizeAttribute as CFString, &sv) == .success,
              let pv, let sv else { return nil }
        var p = CGPoint.zero, s = CGSize.zero
        guard AXValueGetValue(pv as! AXValue, .cgPoint, &p), AXValueGetValue(sv as! AXValue, .cgSize, &s) else { return nil }
        // A whole window or a huge web area is not a useful focus target.
        if s.width <= 0 || s.height <= 0 || (!anySize && s.width * s.height > 1_500_000) { return nil }
        return Rc(l: p.x, t: p.y, r: p.x + s.width, b: p.y + s.height)
    }
}
