// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
#pragma once
// Pure hybrid engine-pick decision (no <windows.h>), extracted so the most regression-prone
// predicate in the app is unit-tested (issues #148 exclusion, #172 shell desktop) instead of
// living inline in two RunTick sites that had to stay identical by hand.
//
// Order of the rules (see ShouldPickTransform): a lost render device -> transform; then a non-primary
// monitor -> render; then capture-protected, renderExclude or rotated output -> transform; then the
// per-window-category preference; then the Auto rule.
//
// The transform engine magnifies the PRIMARY monitor only. Its visual offsets are monitor-local
// (ComputeMagTransform gets the mapper's source rect without the monitor origin) while its input
// transform, edge clip and sprite add the origin, so on another monitor the picture and the input
// mapping describe two different rects (review 2026-10-16 #11). Nothing in the repo pins down DWM's
// coordinate convention for MagSetFullscreenTransform off the primary, so no offset is added there:
// a non-primary session gets render (a rotated or protected secondary shows its known render limits
// instead of a wrong region), and a session only moves to a non-primary monitor when the render
// engine took the retarget (MayRetargetSession).
//
// Auto picks transform on the PRIMARY target monitor for (a) a borderless foreground that covers
// the monitor (games, F11 video: compositor-internal magnification survives a heavy game's present
// load) or (b) the desktop, when desktopTransform=1 (shipped) and the input transform is verified.
// Everything else gets the render engine:
//  - a maximized desktop app covers but keeps its caption -> render (documented trap),
//  - the shell desktop (Win+D) reads as a borderless cover, so it is never a game -> render
//    on the game path (issue #172); only the desktop path may take it,
//  - excluded exes (fullscreen browser video wants a desktop-style cursor) -> render,
//  - learned cursor-shape churners -> render, unless the tdrTest harness forces transform,
//  - any non-primary monitor -> render (no cross-adapter transform chase).
// An explicit Transform category preference skips the churny and input-transform checks.
#include <string>

namespace wind {

// PER-WINDOW-TYPE ENGINE SELECTION. The four categories a user can express a preference for. The
// caller classifies the foreground (Win32 reads live in main.cpp) and passes the preference that
// applies; this header stays pure so the decision remains unit-testable.
enum class WindowCategory { Game, Acrylic, Desktop, Other };

// Auto = the historical automatic behaviour, unchanged. An untouched install is all-Auto.
enum class EnginePref { Auto, Transform, Render };

inline EnginePref ParseEnginePref(const std::string& s) {
    if (s == "transform") return EnginePref::Transform;
    if (s == "render")    return EnginePref::Render;
    return EnginePref::Auto;      // unknown/empty falls back to Auto, never to a hard pin
}

struct EnginePickInputs {
    bool coversMonitor  = false;  // foreground covers the session's target monitor
    bool borderless     = false;  // foreground has no WS_CAPTION
    bool primaryMonitor = false;  // the target monitor is the primary
    bool shellDesktop   = false;  // foreground is the shell desktop window class (issue #172)
    bool excluded       = false;  // exe listed in transformExclude (issue #148)
    bool churny         = false;  // exe learned in churny_apps.txt
    bool tdrHarness     = false;  // cfg.tdrTest > 0: bypass the churny list for field experiments
    // Desktop opt-in (issue #185): transform on the DESKTOP requires both - the user's
    // desktopTransform knob AND a verified MagSetInputTransform (without it, pointer-framework
    // apps have hover dead zones; needs UIAccess - POINTER-HITTEST-FINDINGS.md).
    bool desktopTransformOptIn = false;
    bool inputTransformOk      = false;
    // The user's preference for the category this foreground falls into. Auto = unchanged.
    EnginePref pref = EnginePref::Auto;
    // CAPTURE-PROTECTED CONTENT (DRM: Netflix, Apple TV, PlayReady). Desktop Duplication returns
    // BLACK for these, so the render engine shows nothing at all. Set when the foreground window
    // or any descendant carries a non-zero display affinity - the protection often lives on a
    // child video surface rather than the top-level window, so the caller must walk children.
    bool captureProtected = false;
    // Exe listed in renderExclude: the manual escape hatch for protected apps the affinity probe
    // does not catch. Same effect as captureProtected.
    bool renderExcluded = false;
    // The target output is rotated (portrait 90/270). Desktop Duplication hands back the panel's
    // unrotated surface and the render engine has no rotation path, so it would show a wrong
    // image (review 2026-10-09 M3); DWM's own magnification handles rotation. Same effect as
    // captureProtected.
    bool rotatedOutput = false;
    // The render engine's D3D device is lost and not yet recovered (GPU TDR). Nothing can be drawn
    // by it, so every pick goes to the transform engine until recovery (review 2026-10-09 #13).
    bool renderLost = false;
};

inline bool ShouldPickTransform(const EnginePickInputs& in) {
    // 1. NEVER RENDER wins over everything, including transformExclude. A capture-protected
    //    window renders as a black rectangle, which is a total failure; the transformExclude list
    //    exists to avoid a RARE dwm crash at high zoom that the pan wall and the MPO buster
    //    already mitigate. Netflix inside a browser is both at once, and this is the case that
    //    ordering resolves: black video every time beats an occasional crash risk.
    //    A lost render device is the same hard limit: it cannot draw anything, so transform it is.
    //    The one exception to the primary-only rule: a lost device cannot draw at all, and the
    //    device-lost failover hands every session to transform until recovery.
    if (in.renderLost) return true;
    if (!in.primaryMonitor) return false;   // transform is primary-only (header note)
    if (in.captureProtected || in.renderExcluded || in.rotatedOutput) return true;
    // 2. An explicit user preference for this window category. Transform is still refused off the
    //    primary monitor (no cross-adapter transform chase) and on an excluded exe, because those
    //    are correctness limits rather than taste. The churny list and the input-transform check
    //    are deliberately NOT applied here: they only keep the AUTO pick conservative, and an
    //    explicit per-category Transform choice is the user accepting that trade (review
    //    2026-10-09 #44).
    if (in.pref == EnginePref::Render) return false;
    if (in.pref == EnginePref::Transform) return !in.excluded;
    // 3. Auto: the historical behaviour, unchanged.
    //    The GAME path: a borderless cover that is not the shell desktop (issue #172 - Win+D is
    //    not a game). The DESKTOP path: explicit opt-in + verified input transform; the shell
    //    desktop is fine there (that IS the desktop). Exclusions and the churny list veto both.
    const bool game    = in.coversMonitor && in.borderless && !in.shellDesktop;
    const bool desktop = in.desktopTransformOptIn && in.inputTransformOk;
    return (game || desktop) && !in.excluded && (in.tdrHarness || !in.churny);
}

// Which category a foreground falls into, from signals the caller has already read. Order is
// deliberate: a fullscreen game that also happens to declare a Mica backdrop is a GAME, and the
// shell desktop is never a game (issue #172).
inline WindowCategory ClassifyWindow(bool coversMonitor, bool borderless, bool shellDesktop,
                                     bool hasSystemBackdrop) {
    if (shellDesktop) return WindowCategory::Desktop;
    if (coversMonitor && borderless) return WindowCategory::Game;
    if (hasSystemBackdrop) return WindowCategory::Acrylic;
    return WindowCategory::Other;
}

// THE ENGINE SETTING (`model`), applied live. Both engines are built at startup whatever `model`
// says, so a change takes effect on the next pick without a restart. Auto: the automatic rule above.
// A pinned engine: Transform on the primary monitor (render elsewhere, see the header note); Render
// unless the render engine cannot draw at all (device lost) or its overlay sits on the wrong monitor
// (renderStale), where showing nothing is worse. renderStale only happens on the primary
// (MayRetargetSession keeps a session off a non-primary monitor the render engine refused).
inline bool PickTransformFor(const std::string& model, const EnginePickInputs& in, bool renderStale) {
    if (model == "transform") return in.primaryMonitor || in.renderLost;
    if (model == "render") return in.renderLost || renderStale;
    return ShouldPickTransform(in) || renderStale;
}

// Whether a zoom session may move to another monitor (multiMonitor=1). The primary is always fine:
// either engine can magnify it. A non-primary monitor needs the render engine to have taken the
// retarget; a render engine on another adapter refuses it, and a transform-only process (the render
// engine failed to start) has none, so the session stays where it is.
inline bool MayRetargetSession(bool targetPrimary, bool renderTookIt) {
    return targetPrimary || renderTookIt;
}

}  // namespace wind
