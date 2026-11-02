import Foundation
import CoreGraphics
import QuartzCore

// The compositor zoom: private SkyLight calls that drive the same WindowServer zoom macOS's own
// Accessibility Zoom uses. Verified on macOS 27.2 (2026-10-29): callable from an ordinary process,
// origin is the view CENTRE in global points (top-left origin), readback rounds to whole points.
// The fifth argument (keepPointer) must be true: with false, WindowServer keeps the pointer's
// SCREEN position and moves its desktop position whenever the view moves, so a view that follows
// the pointer feeds back into itself and runs into the corner (measured 2026-10-29).
// Looked up at runtime so a missing symbol degrades to "unavailable" instead of failing to launch.

enum SkyLight {
    private typealias MainConnFn = @convention(c) () -> Int32
    private typealias SetZoomFn = @convention(c) (Int32, UnsafePointer<CGPoint>, Double, Bool, Bool, Double) -> Int32
    private typealias GetZoomFn = @convention(c) (Int32, UnsafeMutablePointer<CGPoint>, UnsafeMutablePointer<Double>, UnsafeMutablePointer<Bool>) -> Int32
    private typealias IsZoomedFn = @convention(c) (Int32, UnsafeMutablePointer<Bool>) -> Int32

    private static let handle = dlopen("/System/Library/PrivateFrameworks/SkyLight.framework/SkyLight", RTLD_LAZY)
    private static func sym<T>(_ name: String, _ t: T.Type) -> T? {
        guard let h = handle, let p = dlsym(h, name) else { return nil }
        return unsafeBitCast(p, to: t)
    }
    private static let mainConn = sym("SLSMainConnectionID", MainConnFn.self)
    private static let setZoom = sym("SLSSetZoomParameters", SetZoomFn.self)
    private static let getZoom = sym("SLSGetZoomParameters", GetZoomFn.self)
    private static let isZoomedFn = sym("SLSIsZoomed", IsZoomedFn.self)

    private typealias CursorVisibleFn = @convention(c) (Int32) -> Bool
    private static let cursorVisibleFn = sym("SLSCursorIsVisible", CursorVisibleFn.self)

    static let cid: Int32 = mainConn?() ?? 0

    // Spaces.
    private typealias SpaceCreateFn = @convention(c) (Int32, Int32, CFDictionary?) -> UInt64
    private typealias SpaceSetAbsFn = @convention(c) (Int32, UInt64, Int32) -> Int32
    private typealias SpaceGetAbsFn = @convention(c) (Int32, UInt64) -> Int32
    private typealias SpaceAddWindowsFn = @convention(c) (Int32, UInt64, CFArray, Int32) -> Int32
    private typealias ShowSpacesFn = @convention(c) (Int32, CFArray) -> Int32
    private static let spaceCreate = sym("SLSSpaceCreate", SpaceCreateFn.self)
    private static let spaceSetAbs = sym("SLSSpaceSetAbsoluteLevel", SpaceSetAbsFn.self)
    private static let spaceGetAbs = sym("SLSSpaceGetAbsoluteLevel", SpaceGetAbsFn.self)
    private static let spaceAddWindows = sym("SLSSpaceAddWindowsAndRemoveFromSpaces", SpaceAddWindowsFn.self)
    private static let showSpaces = sym("SLSShowSpaces", ShowSpacesFn.self)
    private static var promptTier: UInt64 = 0

    /// Keep a window on top while a system prompt is up. True when it took effect.
    static func placeAbovePrompts(windowNumber: Int) -> Bool {
        guard let create = spaceCreate, let setAbs = spaceSetAbs, let getAbs = spaceGetAbs,
              let add = spaceAddWindows, let show = showSpaces, windowNumber > 0 else { return false }
        func const(_ n: String, _ d: Int32) -> Int32 {
            guard let h = handle, let p = dlsym(h, "kSLSSpaceAbsoluteLevel" + n) else { return d }
            return p.load(as: Int32.self)
        }
        let agent = const("SecurityAgent", 200), lock = const("ScreenLock", 300)
        let level = min(agent + 50, lock - 1)
        guard level > agent, level < 750 else { return false }
        if promptTier == 0 {
            promptTier = create(cid, 1, nil)
            guard promptTier != 0, setAbs(cid, promptTier, level) == 0 else { promptTier = 0; return false }
        }
        guard getAbs(cid, promptTier) == level else { return false }
        // Both return an asynchronous operation token, not an error code.
        _ = add(cid, promptTier, [NSNumber(value: windowNumber)] as CFArray, 7)
        _ = show(cid, [NSNumber(value: promptTier)] as CFArray)
        return true
    }

    private typealias SetWinPropFn = @convention(c) (Int32, UInt32, CFString, CFTypeRef) -> Int32
    private static let setWinProp = sym("SLSSetWindowProperty", SetWinPropFn.self)

    /// Screenshot window mode (Cmd+Shift+4, Space) skips this window. It picks the topmost window with
    /// visible pixels under the pointer, click-through or not, and Wind's full-screen cursor panel or
    /// render overlay always has some: it highlighted the whole screen and saved Wind's window
    /// (2026-11-01). `sharingType = .none` did not stop it; screencapture reads this property.
    static func ignoreForWindowPicker(windowNumber: Int) {
        guard windowNumber > 0 else { return }
        _ = setWinProp?(cid, UInt32(windowNumber), "IgnoreForScreencaptureWindowSelection" as CFString, kCFBooleanTrue)
    }

    /// Whether the system cursor is shown, whichever app hid it (a game hides it for mouse-look).
    /// SLSCursorIsVisible also reports the "obscured while typing" flag, and that cached byte goes
    /// stale: a zoom write or a warp reveals the cursor without rewriting it, so it read hidden for
    /// seconds while the cursor was visibly moving (false game locks after typing). Read the hide
    /// flag alone from WindowServer's shared scoreboard page (reverse-engineered 2026-10-31, see
    /// docs/architecture.md); fall back to SLSCursorIsVisible if the layout is not the one checked.
    static var cursorVisible: Bool {
        if forceHidden { return !wantHidden }   // the cursor Wind draws (draw engine, Wind's own cursor)
        return realCursorVisible
    }

    /// The real cursor, also while Wind draws its own (tests).
    static var realCursorVisible: Bool {
        if let s = scoreboard?(), let page = s.pointee { return page.load(fromByteOffset: 0x2f, as: UInt8.self) == 0 }
        return cursorVisibleFn?(cid) ?? true
    }

    private typealias ScoreboardFn = @convention(c) () -> UnsafeMutablePointer<UnsafeMutableRawPointer?>?
    /// CGSScoreboard is internal: its address is taken from the `bl` in exported SLCursorIsVisible,
    /// after checking every instruction word around it (macOS 27.2 layout).
    private static let scoreboard: ScoreboardFn? = {
        guard let h = handle, let f = dlsym(h, "SLCursorIsVisible") else { return nil }
        let a = UInt(bitPattern: f) & 0x0000_7FFF_FFFF_FFFF
        guard let w = UnsafePointer<UInt32>(bitPattern: a) else { return nil }
        // The function's prologue, its call and its flag load, checked before the call target is used.
        guard w[0] == 0xdac1a7fe, w[3] >> 26 == 0x25, w[5] == 0xf9400008, w[6] == 0x3940bd08 else { return nil }
        let imm = Int(Int32(bitPattern: w[3] << 6) >> 6)
        let t = UInt(bitPattern: Int(a) + 12 + imm * 4)
        guard let tw = UnsafePointer<UInt32>(bitPattern: t), tw.pointee == 0xdac1a7fe,
              let raw = UnsafeRawPointer(bitPattern: t) else { return nil }
        return unsafeBitCast(raw, to: ScoreboardFn.self)
    }()

    static var available: Bool { setZoom != nil && getZoom != nil && cid != 0 }

    /// Set the view: centre in global points, scale >= 1. smooth = interpolated (blurry) pixels.
    nonisolated(unsafe) static var traceWrites = false
    /// Called just before each immediate view write with the centre and scale as written: Wind's
    /// own cursor moves to match it first, so the cursor and the zoom change reach the same frame.
    /// (`drawnFollow: false` skips it for a write that the same frame overwrites at once: a carry's
    /// first write. Following it as well meant two commits per frame, and the second could miss.)
    nonisolated(unsafe) static var beforeWrite: ((CGPoint, Double) -> Void)?
    @discardableResult
    /// `glide` > 0 makes WindowServer animate to the new centre through fractional positions on a
    /// 10 ms timer with a cosine ease (the sixth argument; reverse-engineered 2026-10-31). Its
    /// duration is about pi * sqrt(scale * distance) / (1500 * glide), clamped to 0.01-0.5 s.
    static func set(center: CGPoint, scale: Double, smooth: Bool, keepPointer: Bool = true, glide: Double = 0, drawnFollow: Bool = true) -> Bool {
        HIDGain.shared.follow(DrawZoom.enabled ? 1 : scale)   // render does not slow the pointer
        if DrawZoom.enabled { DrawZoom.shared.set(center: center, scale: scale, smooth: smooth); return true }
        guard let f = setZoom else { return false }
        if glide == 0 && drawnFollow { beforeWrite?(center, scale) }
        var c = center
        let r = f(cid, &c, scale, smooth, keepPointer, glide) == 0
        if WindCursor.probe { Log.verbose(String(format: "zw %.6f %.2f %.2f", CACurrentMediaTime(), center.x, center.y)) }
        if traceWrites { let q = Magnifier.pointer() ?? .zero; Log.verbose(String(format: "W %.4f %.4f L %.5f keep %d -> ptr %.4f %.4f", center.x, center.y, scale, keepPointer ? 1 : 0, q.x, q.y)) }
        return r
    }

    struct State { var center: CGPoint; var scale: Double; var smooth: Bool }

    static func get() -> State? {
        if DrawZoom.enabled {
            let d = DrawZoom.shared
            return State(center: d.center, scale: d.visible ? d.scale : 1, smooth: false)
        }
        guard let f = getZoom else { return nil }
        var c = CGPoint.zero, s = 0.0, sm = false
        guard f(cid, &c, &s, &sm) == 0 else { return nil }
        return State(center: c, scale: s, smooth: sm)
    }

    static var isZoomed: Bool {
        if DrawZoom.enabled { return DrawZoom.shared.visible }
        guard let f = isZoomedFn else { return (get()?.scale ?? 1) > 1.0001 }
        var z = false
        return f(cid, &z) == 0 && z
    }

    private typealias SetConnPropFn = @convention(c) (Int32, Int32, CFString, CFTypeRef) -> Int32
    private static let setConnProp = sym("SLSSetConnectionProperty", SetConnPropFn.self)
    private static var cursorHidden = false

    /// Hide or show the system cursor from a background app (Inspect mode). Needs the
    /// SetsCursorInBackground connection property; hide and show stay balanced.
    /// The draw engine keeps the real cursor hidden while it draws its own; Wind's other hide and show
    /// calls then only decide whether the drawn cursor shows.
    nonisolated(unsafe) static var forceHidden = false

    /// What Wind itself wants (caret tracking, Inspect, the hide-cursor bind), in either engine.
    nonisolated(unsafe) private(set) static var wantHidden = false

    static func setCursorHidden(_ hide: Bool, force: Bool = false) {
        wantHidden = hide
        if forceHidden { DrawZoom.shared.pointerHidden = hide; return }   // decides the drawn cursor only
        realHide(hide, force: force)
    }

    /// Draw engine: the real cursor stays hidden while Wind draws its own; re-hidden if macOS showed it.
    static func hideRealCursor() {
        let shown = scoreboard.flatMap { $0() }.flatMap { $0.pointee }.map { $0.load(fromByteOffset: 0x2f, as: UInt8.self) == 0 } ?? false
        realHide(true, force: !cursorHidden || shown)
    }

    /// Draw engine off: the real cursor as Wind wants it.
    static func restoreWanted() { realHide(wantHidden) }

    private static func realHide(_ hide: Bool, force: Bool = false) {
        if hide == cursorHidden && !force { return }
        _ = setConnProp?(cid, cid, "SetsCursorInBackground" as CFString, kCFBooleanTrue)
        if hide {
            // The Dock showed the real cursor over its icons and Wind could not hide it again (owner
            // 2026-11-02: a tiny real cursor over the zoomed Dock). Entering the Dock, the Dock takes the
            // cursor environment and WindowServer force-shows the cursor (hide count to 0) unless some
            // connection has both SetsCursorInBackground and AlwaysOwnsCursor ("zoomwindow-owns-cursor",
            // what macOS's zoom window uses); and while the pointer stays over the Dock (a universal-owner
            // connection), only the foreground app may hide or show it, so the re-hide below is refused
            // (not a region lock: measured). AlwaysOwnsCursor lets Wind set it there and keeps the hide
            // through the Dock's take. Held only while Wind hides the cursor.
            // research/wind-mac/re/2026-11-02-dock-cursor.md
            _ = setConnProp?(cid, cid, "AlwaysOwnsCursor" as CFString, kCFBooleanTrue)
            if cursorHidden { CGDisplayShowCursor(CGMainDisplayID()); Log.verbose("real cursor shown again: re-hidden") }   // after something showed it
            CGDisplayHideCursor(CGMainDisplayID())
        } else {
            CGDisplayShowCursor(CGMainDisplayID())
            _ = setConnProp?(cid, cid, "AlwaysOwnsCursor" as CFString, kCFBooleanFalse)
        }
        cursorHidden = hide
    }

    /// macOS's own zoom back to 1x, whichever engine is selected (draw engine handover).
    static func resetCompositor() {
        HIDGain.shared.follow(1)
        guard let f = setZoom else { return }
        let b = CGDisplayBounds(CGMainDisplayID())
        var c = CGPoint(x: b.midX, y: b.midY)
        _ = f(cid, &c, 1.0, false, true, 0)
    }

    /// Back to 1x (used on quit, on crash and by the watchdog).
    static func reset() {
        let b = CGDisplayBounds(CGMainDisplayID())
        set(center: CGPoint(x: b.midX, y: b.midY), scale: 1.0, smooth: false)
    }
}
