#include "transform_model.h"
#include "transform.h"   // ComputeMagTransform
#include "logging.h"
#include <windows.h>
#include <magnification.h>
#include <cmath>

namespace wind {

// How long the magnification context lingers after a zoom ends. Long enough that zoom-out /
// zoom-in flicks stay instant, short enough that going back to playing is clean almost at once.
static constexpr unsigned long long kIdleReleaseMs = 1200;

void TransformModel::resetTransformState() {
    // Everything the write path caches must be forgotten across a teardown, or the next session
    // compares against values DWM no longer holds and skips the writes that would re-apply them.
    lastLevel_ = 0.0; lastRequestedLevel_ = 0.0;
    lastOffX_ = lastOffY_ = lastTxX_ = lastTxY_ = 0;
    lastChangeMs_ = 0; keepAliveTick_ = 0;
    lastInputXformOn_ = false;
    ixTick_ = 0; ixPending_ = false;
    lastSpriteX_ = INT_MIN; lastSpriteY_ = INT_MIN;
    lastCenterX_ = -1e9; lastCenterY_ = -1e9;   // txSpriteLead velocity baseline (issue #195)
    easeValid_ = false;                          // free-cursor view easing re-seeds per session
    frozenViewValid_ = false;                    // frozen-view diagnostic re-anchors per session
    samplingApplied_ = false;                    // re-apply sampling mode per fresh context
    haveLastClick_ = false;
}

bool TransformModel::ensureMag() {
    if (magUp_) return true;
    LARGE_INTEGER fr, a, b;
    QueryPerformanceFrequency(&fr); QueryPerformanceCounter(&a);
    const bool ok = host_.initialize();
    QueryPerformanceCounter(&b);
    const double ms = double(b.QuadPart - a.QuadPart) * 1000.0 / fr.QuadPart;
    if (!ok) {
        wind::Log(wind::LogLevel::Warn, "transform", "MagInitialize failed; zoom unavailable");
        return false;
    }
    if (ms > 2.0)
        wind::Log(wind::LogLevel::Info, "transform", "MagInitialize took %.1fms", ms);
    magUp_ = true;
    idleSinceMs_ = 0;
    // First-write instrumentation (#191 review). Armed HERE and at session end (setActive(false)),
    // never at setActive(true): on the enter-active tick present() runs BEFORE setActive(true)
    // (reveal-first), so arming there would time the session's SECOND write.
    coldContext_ = true;
    timeFirstWrite_ = true;
    resetTransformState();
    return true;
}

void TransformModel::teardownMag() {
    if (!magUp_) { identityParked_ = false; return; }
    LARGE_INTEGER fr, a, b;
    QueryPerformanceFrequency(&fr); QueryPerformanceCounter(&a);
    // Cursor state FIRST: MagShowSystemCursor needs a live context, so undoing it after
    // MagUninitialize would silently fail and strand the pointer hidden.
    if (cursorHidden_) { MagShowSystemCursor(TRUE); cursorHidden_ = false; }
    if (sprite_) sprite_->hide();
    if (blanker_) blanker_->restore();
    host_.setTransform(1.0f, 0, 0, 0, 0, false);   // leave DWM at identity before releasing
    RECT full{ 0, 0, mon_.w, mon_.h };
    host_.setInputTransform(false, full, full);
    host_.shutdown();                              // MagUninitialize: DWM leaves magnification mode
    mpoGhost_.hide();                              // never stranded shown across a teardown
    magUp_ = false;
    idleSinceMs_ = 0;
    identityParked_ = false;
    resetTransformState();
    QueryPerformanceCounter(&b);
    wind::Log(wind::LogLevel::Info, "transform", "magnification context released in %.1fms",
              double(b.QuadPart - a.QuadPart) * 1000.0 / fr.QuadPart);
}

bool TransformModel::initialize(const MonitorTarget& monitor) {
    mon_ = monitor;
    // NO magnification context at startup, and no warm-up write (issue #148): a live context puts
    // DWM in magnification-aware compositing, which taxes every cursor change any app makes.
    // The context is created on the first zoom and released again once idle (see idleTick).
    // NO launch warm-up write (issue #148, field-measured): the FIRST fullscreen-transform write
    // puts DWM into magnification-aware compositing, and from then on every cursor visibility or
    // shape change an app makes pays that path - a game that hides/shows the pointer on each
    // middle-click (Foundation: 25 visibility flips per test) then hitches while Wind merely
    // RUNS at 1x. Harness: 15 middle-click drags = 24 spike frames with the warm-up, 0 without.
    // The old justification (hiding a ~110ms cold start on the session's first zoom) is not
    // worth taxing every frame of every game the user plays without zooming.
    if (useSprite_) {
        blanker_ = std::make_unique<CursorBlanker>();
        sprite_  = std::make_unique<CursorSprite>(blanker_->originals());
        // P2 experiment (spriteBand16): band 16, positioned in SCREEN space - testing whether
        // high-band windows escape the DWM fullscreen transform (constant-size cursor).
        sprite_->create(spriteBand16_ ? 16 : zorderBand_);
        // Positioning keys off the ACHIEVED band, never the request: a refused band with
        // screen-space positioning would misplace the sprite AND read as a false experiment
        // verdict (the cascade already logs the refusal - band_window.h).
        if (spriteBand16_ && sprite_->usedBand() < 16) {
            spriteBand16_ = false;
            wind::Log(wind::LogLevel::Warn, "transform",
                      "spriteBand16 requested but band 16 refused (got %d) - experiment inert",
                      sprite_->usedBand());
        }
    }
    if (smoothPan_) pin_.create();
    // MPO buster ghost (issue #191): created once at monitor bounds, shown only during MPO-
    // exposed game sessions (main.cpp gates via setMpoBusterWanted). Creation failure is
    // non-fatal: the fail-closed pan walls simply never lift.
    if (!mpoGhost_.create(mon_.x, mon_.y, mon_.w, mon_.h))
        wind::Log(wind::LogLevel::Warn, "transform", "MpoGhost create failed - walls stay up");
    // Input-transform availability (issue #185): MagSetInputTransform's ENABLED publish needs
    // UIAccess, and the hybrid DESKTOP pick must know availability BEFORE any session exists (a
    // transform desktop session without it has the pointer-framework dead zones). Read the
    // process token's UIAccess bit directly - ZERO Magnification calls at startup. Two probe
    // shapes are BANNED here (self-review, rig-measured): a DISABLED MagSetInputTransform
    // succeeds WITHOUT UIAccess (false positive), and any Mag acquire/release at startup runs
    // MagHost::shutdown's identity transform WRITE - violating the no-warm-up law above and
    // resetting a running native Magnifier's zoom. The in-session enabled publish remains the
    // authority: its first failure clears this flag (self-heal in present()).
    {
        HANDLE tok = nullptr;
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
            DWORD uiAccess = 0, len = 0;
            if (GetTokenInformation(tok, TokenUIAccess, &uiAccess, sizeof(uiAccess), &len))
                inputTransformAvailable_ = uiAccess != 0;
            CloseHandle(tok);
        }
        wind::Log(wind::LogLevel::Info, "transform", "input transform %s (token UIAccess=%d)",
                  inputTransformAvailable_ ? "AVAILABLE" : "unavailable",
                  inputTransformAvailable_ ? 1 : 0);
    }
    ready_ = true;
    return true;
}

// Rolling stats for the input-transform publish (issue #189), mirroring noteWrite: this call
// postdated every hitch baseline and had zero timing until now. Logged once a second only when
// something is interesting (slow publish or a failure).
void TransformModel::noteIxWrite(double ms, bool ok) {
    std::lock_guard<std::mutex> lk(statMx_);
    ++ixCount_;
    ixSumMs_ += ms;
    if (ms > ixMaxMs_) ixMaxMs_ = ms;
    if (!ok) ++ixFails_;
    unsigned long long now = GetTickCount64();
    if (ixLogMs_ == 0) ixLogMs_ = now;
    if (now - ixLogMs_ >= 1000) {
        if (ixMaxMs_ > 5.0 || ixFails_ > 0) {
            wind::Log(wind::LogLevel::Info, "ixwrite",
                      "publishes=%d avg=%.2fms MAX=%.1fms fails=%d",
                      ixCount_, ixCount_ ? ixSumMs_ / ixCount_ : 0.0, ixMaxMs_, ixFails_);
        }
        ixLogMs_ = now; ixMaxMs_ = 0.0; ixSumMs_ = 0.0; ixCount_ = 0; ixFails_ = 0;
    }
}

void TransformModel::noteWrite(double ms, bool ok) {
    std::lock_guard<std::mutex> lk(statMx_);
    ++statCount_;
    statSumMs_ += ms;
    if (ms > statMaxMs_) statMaxMs_ = ms;
    if (ms > 5.0) ++statOver5_;
    if (!ok) ++statFails_;
    unsigned long long now = GetTickCount64();
    if (statLogMs_ == 0) statLogMs_ = now;
    if (now - statLogMs_ >= 1000) {
        if (statMaxMs_ > 5.0 || statFails_ > 0) {
            wind::Log(wind::LogLevel::Info, "txwrite",
                      "writes=%d avg=%.2fms MAX=%.1fms over5ms=%d fails=%d",
                      statCount_, statCount_ ? statSumMs_ / statCount_ : 0.0,
                      statMaxMs_, statOver5_, statFails_);
        }
        statLogMs_ = now; statMaxMs_ = 0.0; statSumMs_ = 0.0;
        statCount_ = 0; statOver5_ = 0; statFails_ = 0;
    }
}

// ASYNC WRITES ARE IMPOSSIBLE (tried 2026-07-26, issue #148): the Magnification API is
// thread-affine, so a writer thread's calls ALL FAIL (measured: fails=144/144) - Wind believed
// it was zoomed while DWM applied nothing. It is also pointless: the write call measures
// 0.02ms avg / 0.5ms max, so it never stalls the tick. The hitch is DWM's ASYNCHRONOUS
// re-scale work, addressed by txLevelStep. Writes stay on the tick thread; only the timing
// instrumentation remains.
void TransformModel::writeTransform(float lvl, int offX, int offY, int tx, int ty,
                                    bool fast, bool) {
    LARGE_INTEGER fr, a, b;
    QueryPerformanceFrequency(&fr); QueryPerformanceCounter(&a);
    bool ok = host_.setTransform(lvl, offX, offY, tx, ty, fast);
    QueryPerformanceCounter(&b);
    noteWrite(double(b.QuadPart - a.QuadPart) * 1000.0 / fr.QuadPart, ok);
}

void TransformModel::hideSystemCursor(bool hide) {
    if (!useSprite_ || !blanker_) return;
    if (hide && !ensureMag()) return;   // MagShowSystemCursor needs a live context
    cursorHidden_ = hide;
    // FOLLOW design (issue #148): during NORMAL zoom the real cursor stays visible (DWM shows it
    // magnified) and main.cpp never calls this. It is called only for INSPECT sessions, where the
    // frozen real cursor must vanish under the crosshair: blanker for standard cursors,
    // MagShowSystemCursor for the plane wholesale (app-custom cursors). Both are undone on exit.
    if (hide) { blanker_->blank(); MagShowSystemCursor(FALSE); if (sprite_) sprite_->show(); }
    else      { if (sprite_) sprite_->hide(); MagShowSystemCursor(TRUE); blanker_->restore(); }
}

void TransformModel::setActive(bool active) {
    active_ = active;
    if (active) {
        // Blank the system cursor set BEFORE the magnification context exists (issue #189): the
        // blanker swaps 14 system cursors, and under a LIVE context every cursor change costs a
        // DWM re-composite (the documented per-change tax) - running the burst inside the fresh
        // context stacked ~14 taxed swaps onto the ~36ms context build, exactly the reported
        // zoom-in hitch. Plain SetSystemCursor needs no context, so it is free out here. The
        // present() hide branch keeps its blank() call as the fallback (idempotent) and still
        // owns MagShowSystemCursor + cursorHidden_ bookkeeping.
        if (useSprite_ && blanker_) blanker_->blank();
        // (A sub-pixel "session warm-up" write here was tried and measured WORSE: 4 spike frames
        // per 3 cycles vs 2, and it added zoom-out spikes. Entering magnification costs ~36ms
        // once per zoom-in regardless - that is DWM building its machinery.)
        ensureMag();
        identityParked_ = false;
        idleSinceMs_ = 0;
        return;
    }
    HookPan().active = 0;   // hook-driven pan (#195) stops with the session
    if (!magUp_) return;
    // MPO buster: hide strictly AFTER the identity park below would be wrong - the park writes
    // identity while the game may still be mid-demotion-return; hiding HERE (before the park)
    // is also wrong for the same reason in reverse. Order chosen: park first (identity is a
    // safe value at any plane state), then hide the ghost - the game re-promotes to its plane
    // against a parked-identity transform, never against a live translation.
    // Give the real pointer back the moment the zoom ends (the follow-session sprite above hid
    // it). Done here, while the context is still alive - MagShowSystemCursor needs one.
    if (cursorHidden_) {
        if (sprite_) sprite_->hide();
        MagShowSystemCursor(TRUE);
        cursorHidden_ = false;
    }
    // Unconditional (and idempotent): setActive(true) pre-blanks BEFORE the context exists, so
    // a session that never entered the draw branch (cursorVisibility=never, hide-hotkey) still
    // has blanked system cursors to give back even though cursorHidden_ never went true.
    if (blanker_) blanker_->restore();
    idleSinceMs_ = GetTickCount64();   // start the release countdown (idleTick)
    wind::Log(wind::LogLevel::Info, "txsession", "session end maxLevel=%.2f", sessionMaxLevel_);
    sessionMaxLevel_ = 0.0;
    // Park at EXACT identity right here, at the end of the zoom-out. Returning DWM to identity
    // costs a ~150ms compositor stall no matter when it happens (measured), so pay it while the
    // user is still in zoom motion and expects movement - not 1.2s later while they are playing.
    LARGE_INTEGER fr, pa, pb;
    QueryPerformanceFrequency(&fr); QueryPerformanceCounter(&pa);
    host_.setTransform(1.0f, 0, 0, 0, 0, false);
    QueryPerformanceCounter(&pb);
    identityParked_ = true;
    parkedAtMs_ = GetTickCount64();
    const double parkMs = double(pb.QuadPart - pa.QuadPart) * 1000.0 / fr.QuadPart;
    if (parkMs > 5.0)
        wind::Log(wind::LogLevel::Info, "transform", "identity park took %.1fms", parkMs);
    RECT full{ 0, 0, mon_.w, mon_.h };
    host_.setInputTransform(false, full, full);   // input mapping back to identity at 1x
    pin_.hide();
    mpoGhost_.hide();   // after the identity park: re-promotion happens against a parked value
    timeFirstWrite_ = true;   // next session (warm context) times its first write too
}

void TransformModel::idleTick() {
    if (!magUp_ || active_ || idleSinceMs_ == 0) return;
    const unsigned long long since = GetTickCount64() - idleSinceMs_;
    if (since < (unsigned long long)idleReleaseMs_) return;
    // The identity park already happened at session end (see setActive); releasing the context
    // afterwards measures 1-2ms, so this is just the "user really stopped zooming" delay.
    teardownMag();
}

void TransformModel::present(const MapResult& r, double level, const Config& cfg,
                             const MonitorTarget& mon, const PresentExtras& ex) {
    (void)mon;
    // CENTERED-CURSOR geometry (issue #148 revival). The original model anchored the transform at
    // the cursor (ComputeFixedPointOffset) because a sprite AT the cursor's real position only sits
    // on correct content when T(L) == L. But Wind's identity is the centered cursor, and the render
    // model's mapper already solves the whole centered geometry - including the edge zones, where
    // cursorScreen slides away from the center as the source rect clamps. So use the mapper's
    // CENTERED source rect for the transform and park the sprite at r.cursorScreen (screen px;
    // layered windows composite OUTSIDE the magnification, so screen coords are its native space):
    //   - content at the lens center (r.centerX/Y) displays at screen center; the sprite sits there;
    //   - the hidden REAL cursor is SetCursorPos'd to clickDesktop (= the lens center rounded), so a
    //     click lands exactly on the aimed content - same invariant as the render model;
    //   - at the edges the mapper moves cursorScreen off-center in lockstep with the clamped source,
    //     so sprite, content, and click point stay welded there too.
    // Bonus vs the old anchored design: the sprite now barely MOVES (center, except at edges), which
    // kills the sprite-lags-the-view wobble that plagued the anchored model during pans.
    // KEEP-ALIVE v2 (issue #148 action-start spike): DWM discards its magnification resources
    // when the transform VALUE sits still and pays a ~1fps rebuild on the next real change.
    // v1 jittered the LEVEL by an epsilon - that forced a full re-SCALE of every cached surface
    // every tick, whose cost grows with zoom (constant ~1fps at 16x: the cure was the disease).
    // v2 jitters only the private-channel TRANSLATION by 1 screen px on alternating ticks - a
    // re-COMPOSITE of already-scaled surfaces, cheap at any level - and only while within 1.5s
    // of the last REAL change: brief pauses (the pan-stop-pan pattern) stay hot, while true idle
    // lets DWM park legitimately (one spike after a long idle is acceptable; one per pause was not).
    // Ramp cost limiter (measured: 150-215ms spikes during zoom ramps at high level): every LEVEL
    // change makes DWM re-scale its cached surfaces, and the cost grows with the level. Apply the
    // ramping level at most every 3rd tick (48Hz on a 144Hz panel - visually still a smooth ramp);
    // panning updates stay per-tick at the applied level so the geometry is always consistent.
    // level==lastLevel_ (no ramp) and the 1x reset apply immediately.
    // Level applies STRAIGHT, per tick, continuously (probe-measured over Foundation: a per-tick
    // level ramp through the private channel costs the game ZERO >25ms frames at steady ~14ms
    // frametimes - identical class to the native Magnifier's eased notches). The earlier
    // quantization/divisor machinery created exactly the big discrete jumps that ARE expensive;
    // small continuous deltas are the cheap pattern. (Quantization removed after A/B.)
    // (The old >8x alternate-tick level divisor is GONE: it was a blind TDR mitigation - the
    // resets were root-caused elsewhere, #148 - and it DOUBLED the per-write level step right
    // where each re-scale is most expensive; big discrete jumps are the measured-costly
    // pattern, small continuous ones the cheap one.)
    double applyLevel = level;
    // txLevelStep (issue #148 hitch work): every LEVEL write makes DWM re-scale its cached
    // surfaces asynchronously - that async work is the hitch (the write call itself measures
    // 0.02ms). Skip level updates whose relative change is under the knob while a ramp is
    // running; the ramp's FINAL level always lands (level == the last requested level means
    // the ramp stopped). Pan/translation keeps updating per tick.
    const bool rampStopped = (level == lastRequestedLevel_);
    lastRequestedLevel_ = level;
    if (cfg.txLevelStep > 0 && !rampStopped && lastLevel_ > 1.0 && level > 1.0) {
        const double rel = std::abs(level - lastLevel_) / lastLevel_;
        if (rel < cfg.txLevelStep / 1000.0) applyLevel = lastLevel_;
    }
    // txMaxStepPct: rate-limit the APPLIED level change per tick. Each change makes DWM re-scale
    // its cached surfaces and that cost grows with the level, so an unclamped fast ramp demands
    // the most expensive re-scales back to back exactly at the top - the suspected cause of the
    // occasional huge spike at max zoom. The applied level trails and catches up within a few
    // ticks of the ramp stopping; the source rect below is recomputed for whatever we apply.
    if (cfg.txMaxStepPct > 0 && lastLevel_ > 1.0 && applyLevel > 1.0) {
        const double maxRel = cfg.txMaxStepPct / 1000.0;
        const double up = lastLevel_ * (1.0 + maxRel);
        const double down = lastLevel_ / (1.0 + maxRel);
        if (applyLevel > up) applyLevel = up;
        else if (applyLevel < down) applyLevel = down;
    }
    // txGrid: snap to a fixed GEOMETRIC ladder (1.0 * g^k) so every zoom reuses the same small
    // set of scale factors instead of minting ~200 fresh ones - DWM's per-factor surface cache
    // then hits instead of missing. Applied on the ramp only; the settled level snaps too (a
    // grid level IS the resting level, so the view never drifts off-grid).
    if (cfg.txGrid > 0 && applyLevel > 1.0) {
        const double g = 1.0 + cfg.txGrid / 1000.0;
        const double k = std::log(applyLevel) / std::log(g);
        double snapped = std::pow(g, std::floor(k + 0.5));
        if (snapped < 1.0) snapped = 1.0;
        applyLevel = snapped;
    }
    double srcL = r.srcLeft, srcT = r.srcTop;
    if (applyLevel != level) {
        OffsetF o = ComputeOffsetF(r.centerX, r.centerY, applyLevel, mon_.w, mon_.h);
        srcL = o.x; srcT = o.y;
    }
    // Real-cursor mode (issue #195): the visible pointer is the REAL cursor, DWM-composited
    // magnified (field-verified: native treatment on the public channel). It is WELDED to the
    // integer clickDesktop point - so the transform must be anchored to that SAME integer
    // point, or the +-frac residual between round(cx) and float cx displays as a
    // +-0.5px*level re-centering sawtooth on the cursor (the reported wobble). Anchoring here
    // makes T(weld) == screen centre EXACT by construction; the view inherits the integer
    // grid (level-px steps at slow pan), exactly like native Magnifier's cursor-driven view.
    if (cfg.txCursorProbe == 3 && !ex.cursorLocked) {
        // FROZEN-VIEW DIAGNOSTIC (issue #195): magnify, but never pan - the source rect is
        // captured once and reused. The cursor then glides across a completely static magnified
        // image, which splits the wobble question in two with one look:
        //   cursor glides smoothly  -> the view/pan path owns the wobble (our writes);
        //   cursor still judders    -> DWM's magnified CURSOR rendering owns it, and no amount
        //                              of pan work can ever fix it (native would show it too).
        if (!frozenViewValid_) {
            POINT pc{};
            GetCursorPos(&pc);
            OffsetF o = ComputeOffsetF((double)(pc.x - mon_.x), (double)(pc.y - mon_.y),
                                       applyLevel, mon_.w, mon_.h);
            frozenSrcL_ = std::trunc(o.x); frozenSrcT_ = std::trunc(o.y);
            frozenViewValid_ = true;
        }
        srcL = frozenSrcL_; srcT = frozenSrcT_;
    } else if (cfg.txCursorProbe == 4 && !ex.cursorLocked) {
        // HOOK-DRIVEN PAN (issue #195): the LL mouse hook writes the offset itself, straight
        // from each mouse event (HookPanWrite) - native's architecture exactly. The tick only
        // publishes the session parameters and leaves the pan alone, so there is no second
        // writer on a different clock to beat against.
        HookPanState& hp = HookPan();
        hp.level = applyLevel;
        hp.monX = mon_.x; hp.monY = mon_.y; hp.monW = mon_.w; hp.monH = mon_.h;
        hp.active = 1;
        POINT pc{};
        GetCursorPos(&pc);
        OffsetF o = ComputeOffsetF((double)(pc.x - mon_.x), (double)(pc.y - mon_.y),
                                   applyLevel, mon_.w, mon_.h);
        srcL = std::trunc(o.x); srcT = std::trunc(o.y);
    } else if (cfg.txCursorProbe == 2 && !ex.cursorLocked) {
        // FREE-CURSOR mode (the native design, issue #195): no weld at all - the cursor moves
        // freely under the user's hand, and the VIEW pursues it with EASING. Exact per-tick
        // centering was field-tested and still wobbled: pinning the cursor to center each
        // frame makes the per-composite timing noise between DWM's live cursor sampling and
        // our transform writes the ONLY relative motion left - pure visible jitter. Easing the
        // view (native centered mode does the same) low-passes our src trajectory, so the
        // cursor-vs-view motion is a smooth deliberate glide that swallows the noise: solid at
        // rest, a soft speed-proportional lead in motion. tau is level-normalized so the
        // ON-SCREEN feel is the same at every zoom. Hover/clicks/drags stay native-correct by
        // construction (the cursor IS at its true position - the FOLLOW-era no-dead-zones law).
        POINT pc{};
        GetCursorPos(&pc);
        double px = pc.x - mon_.x, py = pc.y - mon_.y;
        predictCursor(cfg, px, py);   // cancel the fixed cursor-vs-content pipeline latency
        // QPC, not GetTickCount64 (field regression 2026-08-13): tick-count granularity is
        // ~16ms against ~7ms ticks, so dt read 0 and 16 alternately - the easing froze and
        // double-stepped frame to frame, beating against the composite clock (started fine,
        // degraded into snapping and heavy wobble as the phases drifted).
        LARGE_INTEGER eFr, eNow;
        QueryPerformanceFrequency(&eFr);
        QueryPerformanceCounter(&eNow);
        const unsigned long long nowE = (unsigned long long)(eNow.QuadPart * 1000000LL / eFr.QuadPart);
        if (!easeValid_) { easeCx_ = px; easeCy_ = py; easeValid_ = true; lastEaseMs_ = nowE; }
        double dtE = (nowE - lastEaseMs_) / 1e6;   // us -> s
        lastEaseMs_ = nowE;
        if (dtE > 0.05) dtE = 0.05;   // a hitch must not teleport the view
        if (dtE < 0.0) dtE = 0.0;
        const double lvlForTau = applyLevel > 1.0 ? applyLevel : 1.0;
        const double tau = cfg.txFollowEaseMs > 0 ? (cfg.txFollowEaseMs / 1000.0) / lvlForTau : 0.0;
        const double aE = tau > 0.0 ? 1.0 - std::exp(-dtE / tau) : 1.0;
        easeCx_ += (px - easeCx_) * aE;
        easeCy_ += (py - easeCy_) * aE;
        OffsetF o = ComputeOffsetF(easeCx_, easeCy_, applyLevel, mon_.w, mon_.h);
        // Whole-pixel offsets, exactly like native (see fastCursorRepan): no fractional
        // residual means no level-amplified shimmer on the magnified cursor.
        srcL = std::trunc(o.x); srcT = std::trunc(o.y);
    } else if (cfg.txCursorProbe != 0) {
        OffsetF o = ComputeOffsetF((double)r.clickDesktopX, (double)r.clickDesktopY,
                                   applyLevel, mon_.w, mon_.h);
        srcL = o.x; srcT = o.y;
    }
    idleReleaseMs_ = cfg.txIdleReleaseMs;   // hot-reloadable release window
    if (!ensureMag()) return;   // lazy context: the session's first write brings DWM up
    // Bitmap smoothing (issue #195): apply once per context via the safe Magnification.dll
    // ordinal-1 wrapper. Every session that never sets it runs NEAREST (mode 0) - the exact
    // pixelation the field compared against native's crisp cursor/scene; Magnify.exe sets it
    // at startup, which is why launching WM alongside smoothed our session live.
    if (cfg.txSamplingMode >= 0 && !samplingApplied_) {
        samplingApplied_ = true;
        const bool ok = host_.setSamplingMode((unsigned)cfg.txSamplingMode);
        wind::Log(wind::LogLevel::Info, "transform",
                  "bitmap smoothing %d applied=%d", cfg.txSamplingMode, ok ? 1 : 0);
    }
    if (level > sessionMaxLevel_) sessionMaxLevel_ = level;
    // Repan rate proof (issue #195). Logged from the TICK path, not the write path: a counter
    // dumped only when a write happens cannot report the case that matters (writes not
    // happening), and its "per second" would be divided by an unknown interval.
    {
        const unsigned long long nowR = GetTickCount64();
        if (repanLogMs_ == 0) repanLogMs_ = nowR;
        else if (nowR - repanLogMs_ >= 1000) {
            if (repanCalls_ > 0 || repanCount_ > 0)
                wind::Log(wind::LogLevel::Info, "transform",
                          "repan writes=%d/s calls=%d/s dedupe=%d/s hookMoves=%u/s (level=%.2f)",
                          repanCount_, repanCalls_, repanDedupe_, ex.moveSignals, level);
            repanCount_ = 0; repanCalls_ = 0; repanDedupe_ = 0; repanLogMs_ = nowR;
        }
    }
    const bool ramping = applyLevel != level || (applyLevel != lastLevel_ && lastLevel_ > 0.0);
    MagTransform m = ComputeMagTransform(srcL, srcT, applyLevel, mon_.w, mon_.h);
    // 2D write-site 16-bit backstop (issue #191): when the session is MPO-exposed AND the ghost
    // is not verifiably holding the game off its overlay plane, the never-exceed-32767 invariant
    // is enforced HERE, structurally, regardless of the mapper walls (which divide by the
    // CONTROLLER level while this write uses the ramp-limited applyLevel - step-limit drift
    // otherwise spends the headroom on faith). Checked fresh at write time - a ghost that died
    // mid-session re-arms the clamp on the very next write, one tick before the wall re-engages.
    // Both channels recomputed so offsets and translations describe the same rect. MUST share the
    // wall's evidence gate: clamping while the walls are lifted would pin the view at the 32000
    // line while the mapper (and welded cursor) pan on past it.
    if (mpoExposed_ && !mpoGhost_.settled(GetTickCount64())) {
        bool clamped = false;
        if (m.txX < -32000) { m.txX = -32000; clamped = true; }
        if (m.txY < -32000) { m.txY = -32000; clamped = true; }
        if (clamped) {
            m.offX = (int)(-m.txX / applyLevel);
            m.offY = (int)(-m.txY / applyLevel);
        }
    }
    if (cfg.tdrTest == 2) {
        // Overflow probe (issue #148 harness): keep the level-space translation inside a signed
        // 16-bit range. If the far-right max-zoom crashes vanish with this, some DWM/driver
        // layer packs the translation into 16 bits and the real fix is this clamp (or less).
        const int maxOff = (int)(32000.0 / applyLevel);
        if (m.offX > maxOff) m.offX = maxOff;
        if (m.txX < -32000) m.txX = -32000;
    }
    // pauseWrites (issue #148): a click's injected cursor move is in flight - a transform write
    // racing a cursor-position update is the proven TDR, so those ticks write NOTHING. State is
    // untouched; the next unpaused tick lands the same values.
    if (!ex.pauseWrites) {
    const bool changed = m.offX != lastOffX_ || m.offY != lastOffY_ ||
                         m.txX != lastTxX_ || m.txY != lastTxY_ || applyLevel != lastLevel_;
    if (changed) {
        lastOffX_ = m.offX; lastOffY_ = m.offY; lastTxX_ = m.txX; lastTxY_ = m.txY;
        lastLevel_ = applyLevel;
        lastChangeMs_ = GetTickCount64();
        keepAliveTick_ = 0;
    }
    int txJitter = 0;
    // Keep-alive: 700ms window, <=8x only. (The original 1.5s/all-levels spec was re-tried
    // under MPO-off 2026-07-26 and measured WORSE - 360ms worst spike vs 198ms baseline. With
    // MPO disabled the desktop composites in software, so a hot magnification pipeline taxes
    // every frame; keep the window short and the high-zoom pipeline parked.)
    bool keepAliveActive = false;
    if (!changed && !ramping && applyLevel <= (double)cfg.txKeepAliveMaxLevel &&
        GetTickCount64() - lastChangeMs_ < 700) {
        keepAliveTick_ ^= 1;
        txJitter = keepAliveTick_;
        keepAliveActive = true;   // BOTH parities must write (the return-to-true-value half too)
    }
    // Same-value hygiene (issue #189): once the keep-alive window has lapsed (or above its level
    // gate), a zoomed-idle tick would push an identical write 144x/s. DWM parks on static values
    // anyway (measured), so skipping is free; the next changed/keep-alive tick writes as before.
    // Mode 3 (frozen-view diagnostic) forces the write every tick: the dedupe would otherwise
    // stop all writes once the value settles, and the field shows the magnified cursor
    // disappearing when writes stop - so the diagnostic must keep the write stream alive to
    // isolate CURSOR smoothness from PAN smoothness.
    if (changed || keepAliveActive || cfg.txCursorProbe == 3) {
        // First-write instrumentation (#191 review): the ~36ms "DWM builds its machinery" cost was
        // a code-comment estimate with no log evidence - 57% of the field day's zoom-ins were cold
        // and the zoom-in edge is exactly where sluggishness is judged. Make it a number.
        if (timeFirstWrite_) {
            timeFirstWrite_ = false;
            LARGE_INTEGER ffr, fa, fb;
            QueryPerformanceFrequency(&ffr); QueryPerformanceCounter(&fa);
            writeTransform((float)applyLevel, m.offX, m.offY, m.txX + txJitter, m.txY, fastPan_, false);
            QueryPerformanceCounter(&fb);
            wind::Log(wind::LogLevel::Info, "transform", "first write took %.1fms (%s context)",
                      double(fb.QuadPart - fa.QuadPart) * 1000.0 / ffr.QuadPart,
                      coldContext_ ? "cold" : "warm");
            coldContext_ = false;
        } else {
            writeTransform((float)applyLevel, m.offX, m.offY, m.txX + txJitter, m.txY, fastPan_, false);
        }
    }
    // Input transform. Mode 1 (THE SHIPPED DEFAULT; field-verified 4x-20x,
    // POINTER-HITTEST-FINDINGS.md): publish the visual source rect on every change, exactly
    // like native Magnifier. Pointer-framework apps (Explorer/Settings/shell) hit-test mouse
    // input through this; without it the welded cursor has hard hover dead zones. Mode 2 =
    // enabled identity (diagnostic; measured DEAD). Mode 0 = off (diagnostic; measured DEAD).
    // Both rects in VIRTUAL-SCREEN coordinates (the old 0,0-based dst was wrong off-primary).
    // Needs UIAccess: the ENABLED publish fails without it (rig-measured ERROR_ACCESS_DENIED;
    // the DISABLED call succeeds regardless - never probe availability with the disable shape).
    // The magnified CURSOR depends on this publish being kept alive (field-proven by the frozen
    // view diagnostic: stop publishing and the cursor instantly reverts to unmagnified/tiny,
    // even though the view stays magnified). So mode 3 republishes every tick despite nothing
    // changing - and it means a publish GAP is visible as a cursor that drops out of
    // magnification, which is a prime suspect for the field wobble.
    if (cfg.magInputTransform != 0 && (changed || ixPending_ || cfg.txCursorProbe == 3)) {
        // Decimation (issue #189): the publish exists for pointer-framework HOVER hit-testing
        // (clicks ride the welded cursor and never consult it), so it does not need the 144Hz
        // motion rate - every Nth changed tick suffices, with a GUARANTEED publish the moment
        // motion rests (changed goes false with one pending) so a stationary aim is always
        // exact. Halves-or-better the per-tick DWM magnification-message rate during ramps/pans
        // (this call postdates every hitch baseline and was fully uninstrumented until now).
        if (changed) ixPending_ = true;
        const bool rest = !changed;
        // REAL-CURSOR mode publishes on EVERY change (issue #195): DWM positions the magnified
        // cursor from the input transform, so decimating it starves the cursor between view
        // updates - measured, large per-frame cursor lurches fell from 24% of frames to 10%
        // when this went to every-change, and the spread dropped below native's. The #189
        // decimation stays for the welded/sprite designs, where the publish only feeds
        // pointer-framework hover hit-testing and 4x fewer publishes is a free perf win.
        const int ixEvery = (cfg.txCursorProbe == 2) ? 1 : cfg.ixDecimate;
        if (rest || ++ixTick_ >= ixEvery) {
            ixTick_ = 0;
            ixPending_ = false;
            // srcL/srcT, not r.srcLeft/srcTop: when the ramp limiters make applyLevel != level
            // the VISUAL transform uses the recomputed origin, and the input mapping must
            // describe what is actually on screen.
            InputTransformRects ir = ComputeInputTransformRects(
                srcL, srcT, applyLevel, mon_.x, mon_.y, mon_.w, mon_.h);
            RECT dst{ ir.dl, ir.dt, ir.dr, ir.db };
            RECT src = (cfg.magInputTransform == 2) ? dst : RECT{ ir.sl, ir.st, ir.sr, ir.sb };
            const bool enabledPublish = applyLevel > 1.001;
            LARGE_INTEGER fr, a, b;
            QueryPerformanceFrequency(&fr); QueryPerformanceCounter(&a);
            bool ok = host_.setInputTransform(enabledPublish, src, dst);
            QueryPerformanceCounter(&b);
            noteIxWrite(double(b.QuadPart - a.QuadPart) * 1000.0 / fr.QuadPart, ok);
            // Availability latch (issue #185, softened after the #191 field review): the log
            // caught ONE teardown-adjacent enabled publish failing on a UIAccess build (mid
            // zoom-out at ~16x, likely racing the context release), and the old first-failure
            // latch then silently downgraded every desktop session to render until relaunch.
            // Require 2 CONSECUTIVE enabled-publish failures to latch, and let any later
            // enabled-publish SUCCESS re-arm availability (game sessions keep publishing through
            // the latch, so a spurious trip self-heals on the next game zoom). Fail-closed per
            // session is preserved: the desktop pick still never runs without verified success.
            // Only the ENABLED shape counts either way - the disabled publish succeeds without
            // UIAccess (rig-measured) and must never feed the latch.
            if (enabledPublish) {
                if (!ok) {
                    if (++ixConsecFails_ >= 2 && inputTransformAvailable_) {
                        inputTransformAvailable_ = false;
                        if (!inputXformWarned_) {
                            inputXformWarned_ = true;
                            wind::Log(wind::LogLevel::Warn, "transform",
                                      "MagSetInputTransform failed twice (no UIAccess?) - desktop pick disabled");
                        }
                    }
                } else {
                    ixConsecFails_ = 0;
                    if (!inputTransformAvailable_) {
                        inputTransformAvailable_ = true;
                        inputXformWarned_ = false;
                        wind::Log(wind::LogLevel::Info, "transform",
                                  "MagSetInputTransform succeeded - desktop pick re-armed");
                    }
                }
            }
        }
    } else if (changed && lastInputXformOn_) {
        RECT full{ 0, 0, mon_.w, mon_.h };
        host_.setInputTransform(false, full, full);
    }
    if (changed) lastInputXformOn_ = cfg.magInputTransform != 0;
    }   // !ex.pauseWrites
    // FIELD-MEASURED (issue #148, this Windows build): DWM's fullscreen magnification DOES
    // magnify layered windows. So the sprite lives in DESKTOP coordinates at the lens center
    // (clickDesktop): the transform displays it AT the screen center (T(center) == cursorScreen,
    // including the edge zones where the mapper slides both), and it grows with zoom naturally,
    // exactly like the native Magnifier's pointer. No self-scaling (that double-scaled).
    (void)cfg;

    // Weld the hidden OS cursor to the lens point, exactly as RenderEngine::render does. This keeps
    // the scene-locked sprite on the real click point AND keeps RunTick's warp-and-measure pan
    // tracking consistent (RunTick assumes the cursor was moved here each active tick). Deduped so an
    // idle tick injects no synthetic mouse move. Inspect freeze pins the point via ex.clickOverride;
    // otherwise clickDesktop is monitor-local, so add the monitor origin for desktop px.
    // WELD the REAL cursor to the lens point, exactly as the render model does. This is what
    // makes the pointer a genuine cursor: the app sees it move, so hover fires instantly,
    // dragging works, and clicks land where you aim - no synthesized clicks, no sprite standing
    // in for a pointer. Deduped so an idle tick injects nothing. Inspect pins it via
    // clickOverride; otherwise clickDesktop is monitor-local, so add the monitor origin.
    // (History: welding was removed when it was believed to reset the GPU driver. That was
    // measured with MPO enabled AND the native Windows Magnifier running - both since
    // eliminated - so it is being re-tested rather than engineered around. If driver resets
    // return, the weld is the first suspect and docs/HITCH-FINDINGS.md has the bisect.)
    // Drag-follow (#169) suspends the weld exactly like the render engine does: mid-drag the
    // pointer owns the interaction, and welding it back fights the hand. weldedLastFrame_
    // records whether SetCursorPos REALLY ran, so RunTick can baseline on the weld point only
    // when it did (#169 measured-baseline law; assuming it landed is the unstable-servo bug).
    weldedLastFrame_ = false;
    // Free-cursor mode (txCursorProbe=2): NEVER weld - the whole point is that nothing snaps
    // the cursor against the hand. RunTick's oracle baselines on the measured cursor when
    // weldedLastFrame() stays false, so the pipeline stays consistent.
    if (!ex.suppressCursorSync && cfg.txCursorProbe != 2) {
        int cx = ex.clickOverride ? ex.clickDesktopX : (r.clickDesktopX + mon_.x);
        int cy = ex.clickOverride ? ex.clickDesktopY : (r.clickDesktopY + mon_.y);
        if (!haveLastClick_ || cx != lastClickX_ || cy != lastClickY_) {
            SetCursorPos(cx, cy);
            lastClickX_ = cx; lastClickY_ = cy; haveLastClick_ = true;
            weldedLastFrame_ = true;
        }
    }

    // Band-16 screen-space sprite scaling (P2 experiment). FIELD VERDICT 2026-08-13: band-16
    // windows do NOT escape the DWM fullscreen transform on this build - the self-scaled sprite
    // ballooned (double-scaled) and detached from the lens. The desktop-space sub-pixel sprite
    // below is the shipped design; this branch stays only as the diagnostic for re-testing the
    // band question on future Windows builds. Do not re-enable spriteBand16 expecting a win.
    if (spriteBand16_ && sprite_)
        sprite_->setScale(cfg.cursorScaleWithZoom != 0 ? (int)(level + 0.5) : 1);
    if (useSprite_ && sprite_ && ex.cursorLocked && ex.drawCursor) {
        // Inspect mode: the real cursor is frozen at the (overridden) click point, but the thing the
        // user aims with is the LOOK POINT (mapper center). Repaint the sprite as the crosshair (the
        // same design the render model draws) and put it on the look point, NOT on cx/cy - those are
        // pinned to the frozen cursor while Inspect is on. The transform is anchored at the look
        // point (T(L) == L) and this layered window composites unmagnified, so the crosshair sits
        // exactly on the aimed content at any zoom, including the 1x roam. Without this branch the
        // sprite kept drawing the arrow at the frozen point (visible, stationary) and no crosshair
        // existed at all - the transform model used to ignore ex.cursorLocked.
        sprite_->showCrosshair();
        // spriteBand16: the one sprite window lives in the band the experiment put it in, so
        // the crosshair must use the same coordinate space as the marker branch below.
        if (spriteBand16_)
            sprite_->moveTo((int)(r.cursorScreenX + 0.5) + mon_.x,
                            (int)(r.cursorScreenY + 0.5) + mon_.y);
        else
            sprite_->moveTo(r.clickDesktopX + mon_.x, r.clickDesktopY + mon_.y);
        sprite_->keepOnTop();
    } else if (useSprite_ && sprite_ && ex.drawCursor && level > 1.001 && cfg.txCursorProbe != 0) {
        // CURSOR PROBE (issue #195): leave the REAL cursor visible and draw nothing ourselves.
        // Native Magnifier's magnified pointer is the system cursor plane composited by DWM
        // (window-enumeration probe: no cursor window, CURSOR_SHOWING=1) - atomic with the
        // transform and high-quality. Field question this knob answers: does OUR session get
        // that DWM cursor treatment (magnified, at the welded lens point -> screen centre), and
        // does the answer depend on the channel (fastPan)? The weld keeps running either way.
        sprite_->hide();
        if (cursorHidden_) { MagShowSystemCursor(TRUE); cursorHidden_ = false; }
        if (blanker_) blanker_->restore();   // undo the setActive pre-blank (idempotent): the
                                             // probe needs the real arrow visible, not blanks
    } else if (useSprite_ && sprite_ && ex.drawCursor && level > 1.001) {
        // The REAL cursor is welded to the lens point above, so input is entirely native - but
        // the hardware pointer is not magnified and is drawn at its raw desktop position, which
        // reads as a small cursor sitting away from the content it addresses. So hide it and
        // draw the marker at cursorScreen: the screen point where that content actually appears.
        // Composited outside the magnification, so it keeps a CONSTANT on-screen size at every
        // zoom level (the standing product rule).
        // ORDER MATTERS: read the shape verdict FIRST. When the focused app hides its own cursor
        // (games, fullscreen video) refreshShape() reports Hidden every tick; hiding before
        // checking made each such tick run a full blank+restore cycle of every system cursor,
        // and each cursor change costs a DWM re-composite while a magnification context is live
        // (the documented per-change tax) - a steady per-tick oscillation for nothing.
        if (sprite_->refreshShape() == CursorSprite::ShapeStatus::Rendered) {
            if (!cursorHidden_) {
                blanker_->blank();
                MagShowSystemCursor(FALSE);
                cursorHidden_ = true;
            }
            // DESKTOP coords, not screen: DWM magnifies layered windows too, so the sprite must
            // live at the lens point in desktop space - the transform then displays it exactly
            // where that content appears. (Placing it in screen space put it off-screen once
            // transformed, which is why the pointer vanished at high zoom.) The consequence is
            // that the marker grows with the zoom, like the native Magnifier's pointer.
            // P2 experiment (spriteBand16): SCREEN space + band 16 instead - if high-band windows
            // escape the transform, cursorScreen is exactly where the aim point displays, at a
            // constant size (the product rule met on the transform path).
            if (spriteBand16_) {
                const int sx = (int)(r.cursorScreenX + 0.5) + mon_.x;
                const int sy = (int)(r.cursorScreenY + 0.5) + mon_.y;
                if (sx != lastSpriteX_ || sy != lastSpriteY_) {
                    sprite_->moveTo(sx, sy);
                    lastSpriteX_ = sx; lastSpriteY_ = sy;
                }
            } else {
                // Sub-pixel positioning (issue #195, the wobble fix): the CONTINUOUS lens
                // centre, not the rounded click point. The fractional residual is baked into
                // the sprite content and position+content travel in ONE atomic ULW
                // (moveToSubpixel), so the displayed cursor sits exactly at screen centre at
                // any zoom - the integer-window-position quantization that the transform
                // magnified into a +-10px re-centering wobble at 20x is gone, and an integer
                // crossing can never split across composites. The welded REAL cursor stays on
                // clickDesktop (integer) for clicks; the <=0.5px visual offset between them is
                // sub-display-pixel at any level. txSpriteLead (A/B) cancels a constant
                // sprite-vs-transform composite skew, if the field shows one.
                double sxF = r.centerX, syF = r.centerY;
                if (cfg.txSpriteLead != 0.0 && lastCenterX_ > -1e8) {
                    sxF += (r.centerX - lastCenterX_) * cfg.txSpriteLead;
                    syF += (r.centerY - lastCenterY_) * cfg.txSpriteLead;
                }
                lastCenterX_ = r.centerX; lastCenterY_ = r.centerY;
                sprite_->moveToSubpixel(sxF + mon_.x, syF + mon_.y);
            }
            sprite_->show();
            sprite_->keepOnTop();
        } else {
            sprite_->hide();   // shape we cannot render: fall back to the system pointer
            if (cursorHidden_) { MagShowSystemCursor(TRUE); blanker_->restore(); cursorHidden_ = false; }
        }
    } else if (useSprite_ && sprite_) {
        // Reached only when the cursor is not drawn at all this tick (cursorVisibility=never,
        // the hide-cursor hotkey) or at <=1.001x. The welded design draws the sprite in every
        // normal zoomed session; the retired FOLLOW look (visible magnified real cursor) is gone.
        sprite_->hide();
    }

    if (smoothPan_ && level > 1.0) {
        unsigned long long now = GetTickCount64();
        if (now - lastPinAssertMs_ >= 500) { lastPinAssertMs_ = now; pin_.assert_(); }
    } else {
        pin_.hide();
    }
    // MPO buster (issue #191): keep the ghost asserted while wanted (500ms cadence of READS -
    // assert_ is calm and only transacts on first show or a regression). Hidden promptly when
    // the session stops being MPO-exposed (alt-tab to the desktop mid-zoom, knob turned off).
    if (mpoBusterWanted_ && level > 1.001) {
        unsigned long long nowG = GetTickCount64();
        if (nowG - lastGhostAssertMs_ >= 500) { lastGhostAssertMs_ = nowG; mpoGhost_.assert_(); }
        // Settle-state transition log (#191 review): without it there is NO way to tell from the
        // field whether the walls are lifted (ghost holding) or engaged (ghost not settled) - the
        // two mutually exclusive regimes feel completely different at high zoom.
        const bool settledNow = mpoGhost_.settled(nowG);
        if (settledNow != ghostWasSettled_) {
            ghostWasSettled_ = settledNow;
            wind::Log(wind::LogLevel::Info, "transform",
                      settledNow ? "MpoGhost settled - pan walls lifted (full range)"
                                 : "MpoGhost not settled - pan walls engaged");
        }
    } else {
        mpoGhost_.hide();
        ghostWasSettled_ = false;   // next session logs its own first settle (no log here: routine)
    }
}

// Cursor lead prediction (issue #195, the velocity-proportional wobble/lead fix).
//
// Measured dead end that motivates this: repanning at ~300 writes/s (every ~3ms, well inside a
// 7ms frame) changed the error not at all, so the offset DWM uses is NOT stale because of our
// write cadence. What remains is a PIPELINE mismatch - DWM latches the cursor plane late (near
// scanout) while the magnified content it is composited against was produced earlier - so the
// view lags the pointer by velocity * pipelineLatency * level no matter how fresh our write is.
// The error is therefore proportional to hand speed, which is exactly the field report ("more
// dramatic the faster I pan") and why every freshness/easing attempt failed.
//
// A FIXED latency is cancelled by aiming where the cursor WILL be when the frame is displayed:
// aim = position + velocity * txCursorLeadMs. Velocity is estimated from the sampled positions
// with a short EMA (raw per-sample velocity is quantized noise; heavy smoothing would reintroduce
// lag). The lead displacement is capped at a quarter view so a flick can never fling the view.
void TransformModel::predictCursor(const Config& cfg, double& x, double& y) {
    LARGE_INTEGER fr, now;
    QueryPerformanceFrequency(&fr);
    QueryPerformanceCounter(&now);
    const double t = (double)now.QuadPart / (double)fr.QuadPart;
    if (velValid_) {
        const double dt = t - velT_;
        if (dt > 1e-5 && dt < 0.1) {
            const double vx = (x - velX_) / dt, vy = (y - velY_) / dt;
            // EMA over ~25ms of samples: enough to reject per-sample quantization, short
            // enough that the estimate still turns with the hand.
            const double a = dt / (dt + 0.025);
            velEmaX_ += (vx - velEmaX_) * a;
            velEmaY_ += (vy - velEmaY_) * a;
        }
    }
    velX_ = x; velY_ = y; velT_ = t; velValid_ = true;
    if (cfg.txCursorLeadMs == 0) return;   // negative = deliberately TRAIL (native runs ~15ms behind)
    const double lead = cfg.txCursorLeadMs / 1000.0;
    double dx = velEmaX_ * lead, dy = velEmaY_ * lead;
    const double capX = (mon_.w / lastLevel_) / 4.0, capY = (mon_.h / lastLevel_) / 4.0;
    if (dx > capX) dx = capX; else if (dx < -capX) dx = -capX;
    if (dy > capY) dy = capY; else if (dy < -capY) dy = -capY;
    x += dx; y += dy;
}

// High-rate cursor repan (issue #195): recompute the free-cursor offset from the CURRENT
// cursor and write it, between composites. Deliberately minimal - no level ramp, no input
// transform, no sprite, no weld: only the pan value DWM is about to sample, so the offset it
// pairs with the live cursor plane is fresh (the wobble/lead fix). Deduped, so a still hand
// costs one GetCursorPos. Runs only in free-cursor mode on a live, active, >1x session.
bool TransformModel::fastCursorRepan(const Config& cfg) {
    ++repanCalls_;
    if (cfg.txCursorProbe != 2 || !magUp_ || !active_) return false;
    if (lastLevel_ <= 1.001) return false;           // idle / ramping into a session
    if (cfg.txFollowEaseMs > 0) return false;        // easing owns the trajectory in that mode
    POINT pc{};
    if (!GetCursorPos(&pc)) return false;
    // "Active" = the POINTER moved, not merely "we wrote". During a slow pan most 1ms polls find
    // no whole-pixel change yet the hand is clearly moving; keying the caller's idle bail-out on
    // writes made it quit the spin after 3 polls and put the wobble straight back (measured).
    const bool cursorMoved = (pc.x != lastRepanCx_ || pc.y != lastRepanCy_);
    lastRepanCx_ = pc.x; lastRepanCy_ = pc.y;
    double aimX = pc.x - mon_.x, aimY = pc.y - mon_.y;
    predictCursor(cfg, aimX, aimY);
    OffsetF o = ComputeOffsetF(aimX, aimY, lastLevel_, mon_.w, mon_.h);
    // Native's exact arithmetic (disassembly): offset = cursor - trunc(halfScreen / level),
    // TRUNCATED to a whole desktop pixel. Both the cursor plane position and the offset are
    // then integers, so (cursor - offset) is an exact integer and the cursor's magnified
    // screen position carries NO fractional residual at any zoom. A sub-pixel offset leaves
    // frac(off)*level of shimmer that re-randomises on every write - up to 20 screen px at
    // 20x, at our write rate.
    o.x = std::trunc(o.x); o.y = std::trunc(o.y);
    MagTransform m = ComputeMagTransform(o.x, o.y, lastLevel_, mon_.w, mon_.h);
    // Same 16-bit backstop the main write path enforces (issue #191): this path writes the
    // same channel, so it must honour the same never-exceed invariant.
    if (mpoExposed_ && !mpoGhost_.settled(GetTickCount64())) {
        bool clamped = false;
        if (m.txX < -32000) { m.txX = -32000; clamped = true; }
        if (m.txY < -32000) { m.txY = -32000; clamped = true; }
        if (clamped) { m.offX = (int)(-m.txX / lastLevel_); m.offY = (int)(-m.txY / lastLevel_); }
    }
    if (m.offX == lastOffX_ && m.offY == lastOffY_ && m.txX == lastTxX_ && m.txY == lastTxY_) {
        ++repanDedupe_;
        return cursorMoved;   // still "active": keep the caller spinning while the hand moves
    }
    lastOffX_ = m.offX; lastOffY_ = m.offY; lastTxX_ = m.txX; lastTxY_ = m.txY;
    lastChangeMs_ = GetTickCount64();
    keepAliveTick_ = 0;
    writeTransform((float)lastLevel_, m.offX, m.offY, m.txX, m.txY, fastPan_, false);
    // Publish the MATCHING input transform in the same breath (issue #195). DWM positions the
    // magnified cursor from this mapping, so a view write without it moves the scene while the
    // cursor stays placed by the previous rect - the pair desyncs by exactly one repan step,
    // which is the relative jump the field sees while panning. magnify.exe always writes the
    // transform and republishes the input rects together; matching that is the whole point.
    if (cfg.magInputTransform != 0) {
        InputTransformRects ir = ComputeInputTransformRects(
            o.x, o.y, lastLevel_, mon_.x, mon_.y, mon_.w, mon_.h);
        RECT dst{ ir.dl, ir.dt, ir.dr, ir.db };
        RECT src = (cfg.magInputTransform == 2) ? dst : RECT{ ir.sl, ir.st, ir.sr, ir.sb };
        host_.setInputTransform(true, src, dst);
        ixPending_ = false;
        ixTick_ = 0;
    }
    ++repanCount_;
    return true;
}

void TransformModel::shutdown() {
    teardownMag();
    if (sprite_) sprite_->destroy();
    if (blanker_) blanker_->restore();
    pin_.destroy();
    mpoGhost_.destroy();
    ready_ = false;
}
}
