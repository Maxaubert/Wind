import Foundation

// Port of Wind's ZoomController (src/zoom/zoom_controller.*): the zoom level as a function of the
// held zoom binds and the wheel, integrated per display tick.

public enum ZoomDir { case none, zoomIn, zoomOut }

/// Both held is ambiguous, so freeze.
public func resolveDirection(inHeld: Bool, outHeld: Bool) -> ZoomDir {
    if inHeld == outHeld { return .none }
    return inHeld ? .zoomIn : .zoomOut
}

/// At speed 1.0 the magnification doubles this many times per second, independent of maxLevel.
public let kZoomDoublingsPerSecond = 2.5
/// Wheel glide time constant: a notch feels immediate but is still a glide (~95% in 0.25 s).
let kWheelTau = 0.08
/// One wheel notch zooms as far as holding the zoom bind does in this long.
let kWheelNotchSeconds = 0.1

public func wheelNotchStep(speed: Double) -> Double {
    pow(2.0, speed * kZoomDoublingsPerSecond * kWheelNotchSeconds) - 1.0
}

public struct ZoomController {
    public private(set) var minLevel: Double
    public private(set) var maxLevel: Double
    public private(set) var level: Double
    public var direction: ZoomDir = .none

    var target = 0.0            // wheel target level (0 = none)
    var inSpeed = 1.0, outSpeed = 1.0
    var smooth = false
    var accel = 3.0, rampSeconds = 0.6
    var heldIn = 0.0            // continuous seconds zoom-in held (drives the ease-in)
    var rateTau = 0.045         // release glide time constant (0 = dead stop)
    var rate = 0.0              // applied rate, signed doublings/s, glides toward the commanded one

    public init(minLevel: Double = 1.0, maxLevel: Double) {
        self.minLevel = minLevel
        self.maxLevel = maxLevel
        self.level = minLevel
    }

    public var hasTarget: Bool { target > 0 }
    /// True while the level is still changing on its own (wheel glide or release ease-out).
    public var isMoving: Bool { hasTarget || rate != 0 || direction != .none }

    public mutating func setMaxLevel(_ m: Double) {
        maxLevel = max(minLevel, m)
        level = min(level, maxLevel)
    }

    public mutating func setProfile(inSpeed: Double, outSpeed: Double, smooth: Bool, accel: Double, rampSeconds: Double) {
        self.inSpeed = inSpeed; self.outSpeed = outSpeed; self.smooth = smooth
        self.accel = accel; self.rampSeconds = rampSeconds
    }

    public mutating func setEaseOut(seconds: Double) { rateTau = max(0, seconds) }

    /// Move the wheel target by x(1+step) per step (fractional steps allowed: trackpads).
    public mutating func stepTarget(_ steps: Double, step: Double) {
        if steps == 0 || step <= 0 { return }
        let base = target > 0 ? target : level
        target = min(maxLevel, max(minLevel, base * pow(1.0 + step, steps)))
    }

    /// The wheel at the user's zoom speeds: up uses inSpeed, down uses outSpeed.
    public mutating func wheelNotches(_ steps: Double) {
        if steps == 0 { return }
        stepTarget(steps, step: wheelNotchStep(speed: steps > 0 ? inSpeed : outSpeed))
    }

    public mutating func tick(_ dt: Double) {
        if direction != .none { target = 0 }   // a held zoom takes over from the wheel
        if target > 0 && dt > 0 {
            // Exponential approach in log space: equal steps look equal at any zoom.
            let a = 1.0 - exp(-dt / kWheelTau)
            level = exp(log(level) + (log(target) - log(level)) * a)
            if abs(log(level / target)) < 0.002 { level = target; target = 0 }
            level = min(maxLevel, max(minLevel, level))
            rate = 0; heldIn = 0
            return
        }
        if direction == .zoomIn && dt > 0 { heldIn += dt } else if direction != .zoomIn { heldIn = 0 }
        if dt <= 0 { return }

        var commanded = 0.0
        switch direction {
        case .zoomIn:
            var accelMult = 1.0
            if smooth && accel > 1.0 {
                let t = (rampSeconds > 0 && heldIn < rampSeconds) ? heldIn / rampSeconds : 1.0
                let startFrac = 1.0 / accel
                accelMult = startFrac + (1.0 - startFrac) * t
            }
            commanded = inSpeed * accelMult * kZoomDoublingsPerSecond
        case .zoomOut:
            commanded = -outSpeed * kZoomDoublingsPerSecond
        case .none:
            break
        }
        if rateTau <= 0 { rate = commanded } else { rate += (commanded - rate) * (1.0 - exp(-dt / rateTau)) }
        if direction == .none && abs(rate) < 0.05 { rate = 0 }
        if rate == 0 { return }
        level *= pow(2.0, dt * rate)
        level = min(maxLevel, max(minLevel, level))
        if direction == .none && (level >= maxLevel || level <= minLevel) { rate = 0 }
    }

    public mutating func reset() {
        level = minLevel; direction = .none; heldIn = 0; rate = 0; target = 0
    }

    /// Instant snap (quick zoom). Cancels any wheel glide.
    public mutating func setLevel(_ l: Double) {
        target = 0; rate = 0
        level = min(maxLevel, max(minLevel, l))
    }

    /// Glide to a level like a wheel step (quick zoom with smoothing).
    public mutating func glide(to l: Double) {
        target = min(maxLevel, max(minLevel, l))
        rate = 0
    }
}

public struct QuickZoomResult: Equatable {
    public var newLevel: Double
    public var newStored: Double
}

/// Zoomed: snap out to 1x, remembering the level only when above 2x. At 1x: snap in to the
/// remembered level (or the default), clamped to [1, maxLevel].
public func applyQuickZoom(cur: Double, stored: Double, def: Double, maxLevel: Double) -> QuickZoomResult {
    var r = QuickZoomResult(newLevel: cur, newStored: stored)
    if cur > 1.0 + 1e-6 {
        if cur > 2.0 { r.newStored = cur }
        r.newLevel = 1.0
    } else {
        r.newLevel = min(maxLevel, max(1.0, stored > 0 ? stored : def))
    }
    return r
}
