import Foundation
import IOKit.hid

// Raw mouse counts straight from the HID devices, for panning while a game holds the pointer
// (Wind's Raw Input on Windows). Event-tap deltas cannot be used then: Wind's own view writes drag
// the held pointer around and WindowServer reports those as moves with large, unrelated deltas.
// Needs the Input Monitoring permission.

final class RawMouse {
    private var manager: IOHIDManager?
    private var dx = 0.0, dy = 0.0
    private(set) var open = false
    private var tried = false

    static var accessGranted: Bool { IOHIDCheckAccess(kIOHIDRequestTypeListenEvent) == kIOHIDAccessTypeGranted }

    /// Ask macOS for Input Monitoring (shows the system prompt once).
    static func requestAccess() { _ = IOHIDRequestAccess(kIOHIDRequestTypeListenEvent) }

    /// Start listening if permitted. Safe to call repeatedly.
    @discardableResult
    func start() -> Bool {
        if open { return true }
        // One attempt per grant: a retry every tick would build a new manager each frame.
        if tried { return false }
        guard RawMouse.accessGranted else { return false }
        tried = true
        let m = IOHIDManagerCreate(kCFAllocatorDefault, IOOptionBits(kIOHIDOptionsTypeNone))
        // Mice only: a mouse also exposes a Pointer collection with the same X/Y, which counted
        // every movement twice.
        let match = [kIOHIDDeviceUsagePageKey: kHIDPage_GenericDesktop, kIOHIDDeviceUsageKey: kHIDUsage_GD_Mouse]
        IOHIDManagerSetDeviceMatching(m, match as CFDictionary)
        let ref = Unmanaged.passUnretained(self).toOpaque()
        IOHIDManagerRegisterInputValueCallback(m, { ctx, _, _, value in
            guard let ctx else { return }
            let me = Unmanaged<RawMouse>.fromOpaque(ctx).takeUnretainedValue()
            let el = IOHIDValueGetElement(value)
            guard IOHIDElementGetUsagePage(el) == kHIDPage_GenericDesktop, IOHIDElementIsRelative(el) else { return }
            let v = Double(IOHIDValueGetIntegerValue(value))
            switch Int(IOHIDElementGetUsage(el)) {
            case kHIDUsage_GD_X: me.dx += v
            case kHIDUsage_GD_Y: me.dy += v
            default: break
            }
        }, ref)
        IOHIDManagerScheduleWithRunLoop(m, CFRunLoopGetMain(), CFRunLoopMode.defaultMode.rawValue)
        let r = IOHIDManagerOpen(m, IOOptionBits(kIOHIDOptionsTypeNone))
        // kIOReturnExclusiveAccess: some device is seized by another app (Karabiner-Elements seizes
        // the physical mice and re-posts through its virtual one). The others still report.
        if r != kIOReturnSuccess && r != kIOReturnExclusiveAccess {
            Log.error(String(format: "raw mouse: open failed 0x%08x", r))
            return false
        }
        if r == kIOReturnExclusiveAccess { Log.info("raw mouse: some devices are seized by another app; using the rest") }
        manager = m
        open = true
        Log.info("raw mouse input on")
        return true
    }

    /// Counts since the last call.
    func take() -> (dx: Double, dy: Double) {
        defer { dx = 0; dy = 0 }
        return (dx, dy)
    }
}
