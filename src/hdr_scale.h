#pragma once
namespace wind {
// Pure logic (no <windows.h>) for the HDR->SDR tonemap scale. See render_shaders.h for the shader
// side and hdr_info.cpp for the OS query.
//
// scRGB (the FP16 linear Rec.709 surface we capture on an HDR desktop) encodes 80 nits as 1.0.
// Windows' "SDR content brightness" slider sets the white level SDR content is composited at, so
// the tonemap divides by it to land SDR white back on 1.0 before the sRGB encode. DWM then applies
// that SAME white level again when it composites our SDR (BGRA8) overlay, which makes the round
// trip exact - but ONLY while this scale tracks the live slider value. A stale value shows up as a
// brightness step of actual/assumed on every zoom-in and zoom-out (darker below the cached point,
// brighter above it, matching only at it), which is why the white level is re-read per duplication
// rebuild and on a throttle while rendering rather than cached once per device build.

// Tonemap scale for the shader: SDR white -> 1.0. Junk/not-yet-known input falls back to
// passthrough (1.0) rather than a wild multiplier.
inline float ScRgbScale(double sdrWhiteNits) {
    return (sdrWhiteNits > 1.0) ? (float)(80.0 / sdrWhiteNits) : 1.0f;
}

// Fold a fresh OS query into the cached value. A failed/absent read (<= 1 nit) keeps the last known
// good one: snapping to a default on a transient failure would itself be a visible brightness step.
inline double AcceptSdrWhiteNits(double queried, double previous) {
    return (queried > 1.0) ? queried : previous;
}

// Throttle for the live re-read while rendering. The DisplayConfig round trip measures ~0.007 ms
// on this box - cheap, but it is a syscall over global display state and the hot loop earns nothing
// from running it per frame: the slider is a human-speed control, so a few reads a second track a
// drag invisibly. Only the FP16 HDR capture path uses the scale at all, so an SDR desktop never
// pays the query. Unsigned arithmetic on a monotonic tick count, so lastMs == 0 (never sampled)
// fires immediately.
inline bool ShouldRefreshSdrWhite(bool capFp16, unsigned long long nowMs,
                                  unsigned long long lastMs, unsigned long long intervalMs) {
    return capFp16 && (nowMs - lastMs) >= intervalMs;
}
}
