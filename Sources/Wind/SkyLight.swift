import Foundation
import CoreGraphics

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

    /// Whether the system cursor is shown, whichever app hid it (a game hides it for mouse-look).
    static var cursorVisible: Bool { cursorVisibleFn?(cid) ?? true }

    static var available: Bool { setZoom != nil && getZoom != nil && cid != 0 }

    /// Set the view: centre in global points, scale >= 1. smooth = interpolated (blurry) pixels.
    nonisolated(unsafe) static var traceWrites = false
    @discardableResult
    static func set(center: CGPoint, scale: Double, smooth: Bool, keepPointer: Bool = true) -> Bool {
        guard let f = setZoom else { return false }
        var c = center
        let r = f(cid, &c, scale, smooth, keepPointer, 0) == 0
        if traceWrites { let q = CGEvent(source: nil)?.location ?? .zero; Log.verbose(String(format: "W %.4f %.4f L %.5f keep %d -> ptr %.4f %.4f", center.x, center.y, scale, keepPointer ? 1 : 0, q.x, q.y)) }
        return r
    }

    struct State { var center: CGPoint; var scale: Double; var smooth: Bool }

    static func get() -> State? {
        guard let f = getZoom else { return nil }
        var c = CGPoint.zero, s = 0.0, sm = false
        guard f(cid, &c, &s, &sm) == 0 else { return nil }
        return State(center: c, scale: s, smooth: sm)
    }

    static var isZoomed: Bool {
        guard let f = isZoomedFn else { return (get()?.scale ?? 1) > 1.0001 }
        var z = false
        return f(cid, &z) == 0 && z
    }

    private typealias SetConnPropFn = @convention(c) (Int32, Int32, CFString, CFTypeRef) -> Int32
    private static let setConnProp = sym("SLSSetConnectionProperty", SetConnPropFn.self)
    private static var cursorHidden = false

    /// Hide or show the system cursor from a background app (Inspect mode). Needs the
    /// SetsCursorInBackground connection property; hide and show stay balanced.
    static func setCursorHidden(_ hide: Bool, force: Bool = false) {
        if hide == cursorHidden && !force { return }
        _ = setConnProp?(cid, cid, "SetsCursorInBackground" as CFString, kCFBooleanTrue)
        if hide {
            if cursorHidden { CGDisplayShowCursor(CGMainDisplayID()) }   // re-hide after the Dock showed it
            CGDisplayHideCursor(CGMainDisplayID())
        } else {
            CGDisplayShowCursor(CGMainDisplayID())
        }
        cursorHidden = hide
    }

    /// Back to 1x (used on quit, on crash and by the watchdog).
    static func reset() {
        let b = CGDisplayBounds(CGMainDisplayID())
        set(center: CGPoint(x: b.midX, y: b.midY), scale: 1.0, smooth: false)
    }
}
