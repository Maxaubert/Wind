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
        let el = AXUIElementCreateApplication(pid)
        // A hung app must not stall the main thread, which also runs the event tap.
        AXUIElementSetMessagingTimeout(el, 0.25)
        appElement = el
        // Electron apps build their accessibility tree only when asked.
        AXUIElementSetAttributeValue(el, "AXManualAccessibility" as CFString, kCFBooleanTrue)
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

    private func detach() {
        if let obs = observer {
            CFRunLoopRemoveSource(CFRunLoopGetMain(), AXObserverGetRunLoopSource(obs), .defaultMode)
        }
        observer = nil
        appElement = nil
        settleTimer?.invalidate(); settleTimer = nil
        pending = nil; pendingSince = nil
    }

    private func handle(_ notification: String, element: AXUIElement) {
        if notification == kAXFocusedUIElementChangedNotification as String {
            if let r = caretRect(of: focusedElement() ?? element) {
                publish(.caret, r)
            } else if let r = frame(of: focusedElement() ?? element) {
                publish(.focus, r)
            }
        } else {
            if let r = caretRect(of: focusedElement() ?? element) { publish(.caret, r) }
        }
    }

    /// A caret must hold still this long before it is published. Terminal apps (and TUIs inside
    /// them) move the cursor around in bursts while redrawing; only where it settles counts.
    static let settleMs = 60.0
    /// A caret that never holds still (a busy TUI) is still published after this long.
    static let maxWaitMs = 250.0
    private var pending: (TrackKind, Rc)?
    private var pendingSince: Date?
    private var settleTimer: Timer?

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
    private func caretRect(of el: AXUIElement) -> Rc? {
        var rangeVal: CFTypeRef?
        guard AXUIElementCopyAttributeValue(el, kAXSelectedTextRangeAttribute as CFString, &rangeVal) == .success,
              let rangeVal, CFGetTypeID(rangeVal) == AXValueGetTypeID() else { return nil }
        var range = CFRange()
        guard AXValueGetValue(rangeVal as! AXValue, .cfRange, &range) else { return nil }
        if let r = bounds(el, range), r.height > 0 { return r }
        // An empty range often has no bounds: measure the character before the caret instead.
        if range.length == 0 && range.location > 0,
           let r = bounds(el, CFRange(location: range.location - 1, length: 1)), r.height > 0 {
            return Rc(l: r.r, t: r.t, r: r.r + 1, b: r.b)
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

    private func frame(of el: AXUIElement) -> Rc? {
        var pv: CFTypeRef?, sv: CFTypeRef?
        guard AXUIElementCopyAttributeValue(el, kAXPositionAttribute as CFString, &pv) == .success,
              AXUIElementCopyAttributeValue(el, kAXSizeAttribute as CFString, &sv) == .success,
              let pv, let sv else { return nil }
        var p = CGPoint.zero, s = CGSize.zero
        guard AXValueGetValue(pv as! AXValue, .cgPoint, &p), AXValueGetValue(sv as! AXValue, .cgSize, &s) else { return nil }
        // A whole window or a huge web area is not a useful focus target.
        if s.width <= 0 || s.height <= 0 || s.width * s.height > 1_500_000 { return nil }
        return Rc(l: p.x, t: p.y, r: p.x + s.width, b: p.y + s.height)
    }
}
