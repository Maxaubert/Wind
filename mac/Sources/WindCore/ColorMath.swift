// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
import Foundation

// The colour filter's warmth (#597), a port of the Windows src/color/color_matrix.h. Shared cases:
// tests/fixtures/shared/color_cases.txt (SharedFixtures.swift here, test_shared_fixtures.cpp there).

/// Channel gains for a blackbody colour temperature, in LINEAR light, normalised so 6500 K is white
/// and the largest channel is 1 (Planck's law and the CIE 1931 colour-matching functions, converted to
/// linear sRGB); 1000 K to 6500 K in 100 K steps. Float, like the Windows table, so both compute the same.
let kelvinGains: [(Float, Float, Float)] = [
    (1.0000, 0.0303, 0.0000), (1.0000, 0.0532, 0.0000), (1.0000, 0.0774, 0.0000), (1.0000, 0.1024, 0.0000),
    (1.0000, 0.1279, 0.0000), (1.0000, 0.1537, 0.0000), (1.0000, 0.1796, 0.0000), (1.0000, 0.2055, 0.0000),
    (1.0000, 0.2312, 0.0000), (1.0000, 0.2567, 0.0000), (1.0000, 0.2820, 0.0077), (1.0000, 0.3069, 0.0174),
    (1.0000, 0.3315, 0.0283), (1.0000, 0.3557, 0.0404), (1.0000, 0.3794, 0.0538), (1.0000, 0.4027, 0.0682),
    (1.0000, 0.4256, 0.0838), (1.0000, 0.4480, 0.1004), (1.0000, 0.4700, 0.1179), (1.0000, 0.4915, 0.1364),
    (1.0000, 0.5125, 0.1557), (1.0000, 0.5331, 0.1757), (1.0000, 0.5532, 0.1965), (1.0000, 0.5729, 0.2179),
    (1.0000, 0.5920, 0.2400), (1.0000, 0.6108, 0.2625), (1.0000, 0.6291, 0.2856), (1.0000, 0.6469, 0.3091),
    (1.0000, 0.6643, 0.3330), (1.0000, 0.6813, 0.3572), (1.0000, 0.6979, 0.3817), (1.0000, 0.7140, 0.4064),
    (1.0000, 0.7298, 0.4314), (1.0000, 0.7451, 0.4565), (1.0000, 0.7601, 0.4818), (1.0000, 0.7747, 0.5072),
    (1.0000, 0.7889, 0.5326), (1.0000, 0.8028, 0.5581), (1.0000, 0.8163, 0.5836), (1.0000, 0.8295, 0.6091),
    (1.0000, 0.8423, 0.6346), (1.0000, 0.8548, 0.6599), (1.0000, 0.8670, 0.6852), (1.0000, 0.8789, 0.7104),
    (1.0000, 0.8904, 0.7355), (1.0000, 0.9017, 0.7605), (1.0000, 0.9127, 0.7853), (1.0000, 0.9234, 0.8099),
    (1.0000, 0.9339, 0.8344), (1.0000, 0.9441, 0.8587), (1.0000, 0.9540, 0.8828), (1.0000, 0.9637, 0.9067),
    (1.0000, 0.9731, 0.9303), (1.0000, 0.9823, 0.9538), (1.0000, 0.9913, 0.9770), (1.0000, 1.0000, 1.0000),
]
let kelvinMin = 1000.0, kelvinStep = 100.0

/// Gains for any Kelvin, linear between the table's rows and clamped to its ends.
public func kelvinGainsLinear(_ kelvin: Double) -> (r: Double, g: Double, b: Double) {
    let last = Double(kelvinGains.count - 1)
    let f = min(last, max(0, (kelvin - kelvinMin) / kelvinStep))
    let i = Int(f), j = min(i + 1, kelvinGains.count - 1), t = f - Double(i)
    let a = kelvinGains[i], c = kelvinGains[j]
    func mix(_ x: Float, _ y: Float) -> Double { Double(x) + (Double(y) - Double(x)) * t }
    return (mix(a.0, c.0), mix(a.1, c.1), mix(a.2, c.2))
}

public func srgbEncode(_ c: Double) -> Double { c <= 0.0031308 ? 12.92 * c : 1.055 * pow(c, 1 / 2.4) - 0.055 }
public func srgbDecode(_ c: Double) -> Double { c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4) }

/// Warmth 0..1 in two halves (owner 2026-11-21 on Windows: "make it go further", the old 100% now at
/// 50%). 0..0.5 is Windows Night light's own scale over half the slider: 6500 K to 1200 K, LINEAR in
/// Kelvin. 0.5..1 goes past where the table ends: the green 1200 K leaves fades out evenly in
/// sRGB-encoded terms, so 1 is pure red. Blue is already 0 at 1200 K.
public let warmKelvinEnd = 1200.0
public func warmKelvin(_ warm01: Double) -> Double {
    let h = warm01 >= 0.5 ? 1.0 : warm01 * 2
    return 6500 - (6500 - warmKelvinEnd) * h
}

public func warmGainsLinear(_ warm01: Double) -> (r: Double, g: Double, b: Double) {
    let k = kelvinGainsLinear(warmKelvin(warm01))
    if warm01 <= 0.5 { return k }
    let t = warm01 >= 1 ? 1.0 : (warm01 - 0.5) * 2
    return (k.r, srgbDecode(srgbEncode(k.g) * (1 - t)), srgbDecode(srgbEncode(k.b) * (1 - t)))
}

/// Each channel's scale on sRGB-ENCODED values, for warmth 0..1 and dim 0..1 (1 = no dimming): the
/// Windows SDR colour matrix's diagonal at saturation 100%, and on the Mac the display transfer
/// formula's max per channel (that formula acts on the encoded framebuffer values). Out of range clamps.
public func warmGainsSrgb(warm01: Double, dim01: Double) -> (r: Double, g: Double, b: Double) {
    let w = min(1, max(0, warm01)), d = min(1, max(0.01, dim01))
    if w <= 0 { return (d, d, d) }
    let g = warmGainsLinear(w)
    return (srgbEncode(g.r) * d, srgbEncode(g.g) * d, srgbEncode(g.b) * d)
}
