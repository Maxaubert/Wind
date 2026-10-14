#pragma once
// MPO nearest guard (issue #369) - PURE, no <windows.h>, tests/test_mpo_guard.cpp.
//
// Nearest sampling with MPO on is the NVIDIA 16-bit TDR combo (05-transform-engine.md): with no
// layer above it, DWM keeps flip-model surfaces (games, video, browsers) as hardware-plane
// candidates while zoomed, and a candidate's destination rectangle (about -source offset x zoom)
// overflows a 16-bit driver field past ~32767. Smooth sampling never hits it because the resample
// property makes the scaled desktop visual require an EXTERNAL LAYER, and nothing under such a
// visual is ever recorded as a plane candidate (COcclusionContext::PreSubgraph /
// CheckAndRecordOverlayCandidate). A colour transform on that visual does the same. So while zoomed
// at nearest with MPO on, Wind applies a colour effect that is visibly a no-op but not identity:
// DWM composes the zoomed desktop itself, keeps nearest sampling, and no plane can overflow.
// UNVERIFIED on an MPO-on boot: Wind's pan walls stay armed until it is (fail-closed), and nearest
// on an MPO boot needs mpoNearestGuard=1.
#include "color_matrix.h"
namespace wind {

// 0.998 on R, G and B: at most half an 8-bit step darker, below what a viewer can see, and far
// outside any exact-identity test (DWM sends the matrix as-is; IsIdentity tolerates 1e-6).
inline ColorMatrix MpoGuardMatrix() {
    ColorMatrix m = IdentityColorMatrix();
    m.m[0][0] = m.m[1][1] = m.m[2][2] = 0.998f;
    return m;
}

// Only while zoomed: at 1x a colour transform would cost every full-screen app its Independent Flip.
inline bool WantMpoGuard(bool zoomed, bool transformSession, bool mpoOnAtBoot, int effectiveSampling) {
    return zoomed && transformSession && mpoOnAtBoot && effectiveSampling == 0;
}

// The matrix DWM should get: a real filter already forces the layer, so it is used unchanged.
inline ColorMatrix GuardedColorMatrix(const ColorMatrix& wanted, bool guard) {
    return (guard && IsIdentity(wanted)) ? MpoGuardMatrix() : wanted;
}

// Pure core of MpoGhost::settled (comp_pin.cpp reads the window facts, this decides). Fail-closed:
// the walls lift only when every fact checks out and the settle window has elapsed.
constexpr unsigned long long kMpoSettleMs = 350;   // plane-demotion settle window
inline bool MpoGhostSettled(bool created, bool shown, unsigned long long shownAtMs,
                            unsigned long long nowMs, bool windowVisible, bool rectKnown,
                            bool rectMatches) {
    if (!created || !shown || shownAtMs == 0) return false;
    if (nowMs - shownAtMs < kMpoSettleMs) return false;
    if (!windowVisible) return false;
    return rectKnown && rectMatches;
}
}  // namespace wind
