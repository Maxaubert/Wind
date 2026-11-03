import Foundation

// Full pointer speed under macOS's zoom (Transform). WindowServer adds mouse motion in zoomed screen
// space, so the pointer covers 1/level of the desktop per hand movement. In linear mode (pointer
// acceleration off, which LinearMouse or the macOS switch sets) hidd multiplies every delta by the
// mouse's HIDMouseAcceleration before WindowServer sees it: Wind sets that to base * level and the
// two cancel. Nothing reads or moves the pointer. Research (2026-11-01):
// ~/Documents/Claude/research/wind-mac/2026-11-01-disable-zoom-slowdown.md

@_silgen_name("IOHIDEventSystemClientCreate") private func hidClientCreate(_ a: CFAllocator?) -> Unmanaged<CFTypeRef>?
@_silgen_name("IOHIDEventSystemClientCreateSimpleClient") private func hidSimpleClientCreate(_ a: CFAllocator?) -> Unmanaged<CFTypeRef>?
@_silgen_name("IOHIDEventSystemClientCopyServices") private func hidCopyServices(_ c: CFTypeRef) -> Unmanaged<CFArray>?
@_silgen_name("IOHIDServiceClientCopyProperty") private func hidCopyProperty(_ s: CFTypeRef, _ k: CFString) -> Unmanaged<CFTypeRef>?
@_silgen_name("IOHIDServiceClientSetProperty") private func hidSetProperty(_ s: CFTypeRef, _ k: CFString, _ v: CFTypeRef) -> Bool
@_silgen_name("IOHIDEventSystemClientScheduleWithDispatchQueue") private func hidSchedule(_ c: CFTypeRef, _ q: DispatchQueue)
@_silgen_name("IOHIDServiceClientGetRegistryID") private func hidRegistryID(_ s: CFTypeRef) -> Unmanaged<CFTypeRef>?

final class HIDGain {
    static let shared = HIDGain()

    /// Main thread: follow the level (config fullPointerSpeed, Transform without keepPointerSpeed).
    var enabled = false { didSet { if !enabled { follow(1) } } }
    /// Main thread: the gain last asked for (1 = off). Inspect divides it back out.
    private(set) var gain = 1.0

    private static let key = "HIDMouseAcceleration" as CFString
    /// Bases of the mice Wind has raised, so WindGuard or the next launch can put them back.
    private static let saveURL = FileManager.default.homeDirectoryForCurrentUser
        .appendingPathComponent("Library/Application Support/Wind/hidgain.json")

    // Below: the queue only. A set is a synchronous call into hidd (1 to 3 ms), kept off the main thread.
    private let q = DispatchQueue(label: "wind.hidgain")
    private var client: CFTypeRef?
    private var mice: [UInt64: (svc: CFTypeRef, base: Int, written: Int)] = [:]
    private var want = 1.0
    private var scheduled = false
    private var timer: DispatchSourceTimer?
    private var polls = 0

    /// Called with every view write's level. Coalesced: the queue applies only the latest one.
    func follow(_ level: Double) {
        let g = enabled ? max(1, level) : 1
        guard abs(g - gain) > 1e-5 else { return }
        gain = g
        q.async { [self] in
            want = g
            if scheduled { return }
            scheduled = true
            q.async { [self] in scheduled = false; apply() }
        }
    }

    /// Put every raised mouse back now (quit).
    func restoreNow() {
        gain = 1
        q.sync { want = 1; apply() }
    }

    /// Bases left behind by a crash: WindGuard and the next launch put them back.
    static func restoreSaved() {
        guard let d = try? Data(contentsOf: saveURL),
              let saved = try? JSONSerialization.jsonObject(with: d) as? [String: Int], !saved.isEmpty else { return }
        if let c = hidSimpleClientCreate(nil)?.takeRetainedValue() {
            for s in services(c) {
                if let rid = registryID(s), let b = saved[String(rid)] { _ = hidSetProperty(s, key, b as NSNumber) }
            }
        }
        try? FileManager.default.removeItem(at: saveURL)
        Log.info("pointer speed: restored \(saved.count) mice after a crash")
    }

    private func apply() {
        if want <= 1.00001 {
            for (_, m) in mice where read(m.svc) == m.written { _ = hidSetProperty(m.svc, Self.key, m.base as NSNumber) }
            mice = [:]
            save()
            stopPolling()
            return
        }
        if mice.isEmpty { scan() }
        for (rid, m) in mice { write(rid, m) }
        startPolling()
    }

    /// Linear-mode mice; others (curve mode, trackpads) keep macOS's speed.
    private func scan() {
        if client == nil {
            client = (hidClientCreate(nil) ?? hidSimpleClientCreate(nil))?.takeRetainedValue()
            // Scheduled, so its service list follows the system. Unscheduled, it kept the mice of the
            // moment it was made: a mouse that came back as a new service (wake from sleep, a wireless
            // mouse reconnecting) was never raised and Transform stayed at macOS's slow zoomed speed
            // until Wind restarted (owner 2026-11-03, noticed on a live switch from Render).
            if let c = client { hidSchedule(c, q) }
        }
        guard let c = client else { return }
        var found = 0
        for s in Self.services(c) {
            guard let rid = Self.registryID(s), mice[rid] == nil,
                  (number(s, "PrimaryUsagePage") == 1 && number(s, "PrimaryUsage") == 2),
                  number(s, "HIDUseLinearScalingMouseAcceleration") ?? 0 != 0,
                  (hidCopyProperty(s, "HIDPointerAccelerationType" as CFString)?.takeRetainedValue() as? String)
                    ?? (Self.key as String) == Self.key as String,
                  let base = read(s), base > 0 else { continue }
            mice[rid] = (s, base, base)
            found += 1
        }
        if found > 0 { save(); Log.info("pointer speed: \(mice.count) linear mice follow the zoom") }
    }

    private func write(_ rid: UInt64, _ m: (svc: CFTypeRef, base: Int, written: Int)) {
        let v = Int((Double(m.base) * want).rounded())
        if v != m.written || read(m.svc) != v { _ = hidSetProperty(m.svc, Self.key, v as NSNumber) }
        mice[rid] = (m.svc, m.base, v)
    }

    /// LinearMouse puts its own value back when a mouse becomes the active one, the System Settings
    /// slider writes every mouse, and no notification reports either: read back every 100 ms while
    /// zoomed and take a changed value as the new base. A new mouse is picked up within a second.
    private func startPolling() {
        guard timer == nil else { return }
        let t = DispatchSource.makeTimerSource(queue: q)
        t.schedule(deadline: .now() + .milliseconds(100), repeating: .milliseconds(100))
        t.setEventHandler { [self] in
            polls += 1
            for (rid, m) in mice {
                guard let v = read(m.svc) else { mice[rid] = nil; continue }   // unplugged
                if v != m.written {
                    Log.verbose("pointer speed: mouse \(rid) reset to \(v), raising again")
                    mice[rid] = (m.svc, v, v)
                    write(rid, mice[rid]!)
                    save()
                }
            }
            if polls % 10 == 0 { scan(); for (rid, m) in mice where m.written == m.base { write(rid, m) } }
        }
        t.resume()
        timer = t
    }

    private func stopPolling() { timer?.cancel(); timer = nil }

    private func save() {
        let d = Dictionary(uniqueKeysWithValues: mice.map { (String($0.key), $0.value.base) })
        if d.isEmpty { try? FileManager.default.removeItem(at: Self.saveURL); return }
        try? JSONSerialization.data(withJSONObject: d).write(to: Self.saveURL, options: .atomic)
    }

    private func read(_ s: CFTypeRef) -> Int? { number(s, Self.key as String) }

    private func number(_ s: CFTypeRef, _ k: String) -> Int? {
        (hidCopyProperty(s, k as CFString)?.takeRetainedValue() as? NSNumber)?.intValue
    }

    private static func services(_ c: CFTypeRef) -> [CFTypeRef] {
        (hidCopyServices(c)?.takeRetainedValue() as? [AnyObject]) ?? []
    }

    private static func registryID(_ s: CFTypeRef) -> UInt64? {
        (hidRegistryID(s)?.takeUnretainedValue() as? NSNumber)?.uint64Value
    }
}
