#include "transform_model.h"
#include "transform.h"   // ComputeMagTransform
#include "tx_cadence.h"  // ShouldWriteTransform (pure, tested)
#include "logging.h"
#include <windows.h>
#include <magnification.h>
#include <cmath>

namespace wind {

// How long a write may be held back by the cadence gates before it goes out anyway (issue #204).
// Without this a sub-threshold residual movement at the end of a pan would never be written and
// the view would rest up to txMinOffsetPx off where the cursor actually is.
static const unsigned long long kSettleMs = 100;

// How long the magnification context lingers after a zoom ends. Long enough that zoom-out /
// zoom-in flicks stay instant, short enough that going back to playing is clean almost at once.
static constexpr unsigned long long kIdleReleaseMs = 1200;

void TransformModel::resetTransformState() {
    // Everything the write path caches must be forgotten across a teardown, or the next session
    // compares against values DWM no longer holds and skips the writes that would re-apply them.
    lastLevel_ = 0.0; lastRequestedLevel_ = 0.0;
    lastOffX_ = lastOffY_ = lastTxX_ = lastTxY_ = 0;
    lastChangeMs_ = 0; lastWriteMs_ = 0; keepAliveTick_ = 0; hiRampTick_ = 0;
    lastInputXformOn_ = false;
    ixTick_ = 0; ixPending_ = false;
    lastSpriteX_ = INT_MIN; lastSpriteY_ = INT_MIN;
    haveLastClick_ = false;
    samplingApplied_ = false;   // re-apply bitmap smoothing on the next context (DWM-global state)
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
        const int wantBand = spriteBand16_ ? 16 : zorderBand_;
        sprite_->create(wantBand);
        // ALWAYS log the achieved band, not only for the spriteBand16 experiment. band_window.h
        // exists because a silently refused band "looked exactly like success" on #162, and the
        // sprite was still doing exactly that: a zorderBand=16 test against #215 could not tell
        // "band 16 did not help" from "band 16 was never granted". Rig-measured with this in
        // place: 16 IS granted, and every band above it (17-20) is refused on Windows 11 26200,
        // so 16 is the ceiling and the sprite cannot be lifted over a DWM-composited thumbnail.
        if (wantBand > 0) {
            wind::Log(sprite_->usedBand() == wantBand ? wind::LogLevel::Info : wind::LogLevel::Warn,
                      "transform", "cursor sprite band: requested %d, got %d%s",
                      wantBand, sprite_->usedBand(),
                      sprite_->usedBand() == wantBand ? "" : " (REFUSED - cascaded down)");
        }
        // Positioning keys off the ACHIEVED band, never the request: a refused band with
        // screen-space positioning would misplace the sprite AND read as a false experiment
        // verdict.
        if (spriteBand16_ && sprite_->usedBand() < 16) {
            spriteBand16_ = false;
            wind::Log(wind::LogLevel::Warn, "transform",
                      "spriteBand16 requested but band 16 refused (got %d) - experiment inert",
                      sprite_->usedBand());
        }
    }
    if (smoothPan_) pin_.create();
    // MPO buster ghost (issue #191, scope widened in #197): created once at monitor bounds,
    // shown during any MPO-exposed transform session (main.cpp gates via setMpoBusterWanted) -
    // browsers and desktop windows ride overlay planes too, not only games. Creation failure is
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
    idleReleaseMs_ = cfg.txIdleReleaseMs;   // hot-reloadable release window
    if (!ensureMag()) return;   // lazy context: the session's first write brings DWM up
    // Bitmap smoothing, once per magnification context. Without it DWM magnifies with nearest
    // neighbour and the whole view - cursor included - is blocky; native Magnifier sets this at
    // startup, which is why running it alongside used to smooth our session too. The flag is
    // DWM-global and dies with a DWM restart, hence per-context rather than once per process.
    if (cfg.txSamplingMode >= 0 && !samplingApplied_) {
        samplingApplied_ = true;
        const bool ok = host_.setSamplingMode((unsigned)cfg.txSamplingMode);
        wind::Log(wind::LogLevel::Info, "transform", "bitmap smoothing %d applied=%d",
                  cfg.txSamplingMode, ok ? 1 : 0);
    }
    if (level > sessionMaxLevel_) sessionMaxLevel_ = level;
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
    const unsigned long long nowMs = GetTickCount64();
    const bool changed = m.offX != lastOffX_ || m.offY != lastOffY_ ||
                         m.txX != lastTxX_ || m.txY != lastTxY_ || applyLevel != lastLevel_;

    // WRITE CADENCE (issue #204). Traced against native Magnifier: it writes ~59/s while ramping
    // and ~49/s while panning, in ~2.24px steps. We wrote 120/s and 92/s in 1.41px steps, with a
    // THIRD of all writes moving the image by exactly one pixel - because we wrote per tick on a
    // 144Hz panel. Our timing was MORE regular than native's (p95 interval 7.56ms vs 31.44ms), so
    // the surplus was not buying smoothness; every write makes DWM redo work proportional to the
    // level, and we were saturating it. These two gates COALESCE writes - they never drop a
    // destination state, because the next tick recomputes from the same mapper.
    // The decision itself is pure and unit-tested (src/tx_cadence.h, tests/test_tx_cadence.cpp) -
    // the escapes that stop a gate stranding the view are exactly the kind of thing that is easy
    // to get wrong once and never notice.
    TxCadenceIn ci;
    ci.changed          = changed;
    ci.levelMoved       = applyLevel != lastLevel_;
    ci.rampStopped      = rampStopped;
    ci.applyLevel       = applyLevel;
    // DESTINATION space: tx is screen pixels, whereas offX is SOURCE pixels, where at 20x a 1px
    // step is a 20px jump on screen. Thresholding the wrong one would gate ~nothing at high zoom.
    {
        int dtx = m.txX - lastTxX_; if (dtx < 0) dtx = -dtx;
        int dty = m.txY - lastTxY_; if (dty < 0) dty = -dty;
        ci.dMoveDest = dtx > dty ? dtx : dty;
    }
    ci.sinceLastWriteMs = nowMs - lastWriteMs_;
    ci.writeHz          = cfg.txWriteHz;
    ci.minOffsetPx      = cfg.txMinOffsetPx;
    ci.settleMs         = kSettleMs;
    const bool writeNow = ShouldWriteTransform(ci);

    if (writeNow) {
        lastOffX_ = m.offX; lastOffY_ = m.offY; lastTxX_ = m.txX; lastTxY_ = m.txY;
        lastLevel_ = applyLevel;
        lastChangeMs_ = nowMs;
        lastWriteMs_ = nowMs;
        keepAliveTick_ = 0;
    }
    // Everything below keys off whether the write ACTUALLY goes out this tick, not merely whether
    // the values differ - the cached last* state must never claim a write we suppressed.
    const bool changedAndWriting = writeNow;
    int txJitter = 0;
    // Keep-alive: 700ms window, <=8x only. (The original 1.5s/all-levels spec was re-tried
    // under MPO-off 2026-07-26 and measured WORSE - 360ms worst spike vs 198ms baseline. With
    // MPO disabled the desktop composites in software, so a hot magnification pipeline taxes
    // every frame; keep the window short and the high-zoom pipeline parked.)
    // Ships OFF (txKeepAliveMaxLevel now defaults to 0, issue #204): this deliberately wrote a
    // value 1px off the truth, 144x/s, through every pause in a pan. Native Magnifier does the
    // opposite - when the view is static it stops writing entirely.
    bool keepAliveActive = false;
    if (!changedAndWriting && !ramping && cfg.txKeepAliveMaxLevel > 0 &&
        applyLevel <= (double)cfg.txKeepAliveMaxLevel &&
        nowMs - lastChangeMs_ < 700) {
        keepAliveTick_ ^= 1;
        txJitter = keepAliveTick_;
        keepAliveActive = true;   // BOTH parities must write (the return-to-true-value half too)
    }
    // Same-value hygiene (issue #189): once the keep-alive window has lapsed (or above its level
    // gate), a zoomed-idle tick would push an identical write 144x/s. DWM parks on static values
    // anyway (measured), so skipping is free; the next changed/keep-alive tick writes as before.
    // suppressTransformWrite: the mouse hook is the single writer this session (issue #206). The
    // state above is still maintained, so turning the hook path off mid-session resumes cleanly.
    if ((changedAndWriting || keepAliveActive) && !ex.suppressTransformWrite)
        writeTransform((float)applyLevel, m.offX, m.offY, m.txX + txJitter, m.txY, fastPan_, false);
    // Input transform. Mode 1 (THE SHIPPED DEFAULT; field-verified 4x-20x,
    // POINTER-HITTEST-FINDINGS.md): publish the visual source rect on every change, exactly
    // like native Magnifier. Pointer-framework apps (Explorer/Settings/shell) hit-test mouse
    // input through this; without it the welded cursor has hard hover dead zones. Mode 2 =
    // enabled identity (diagnostic; measured DEAD). Mode 0 = off (diagnostic; measured DEAD).
    // Both rects in VIRTUAL-SCREEN coordinates (the old 0,0-based dst was wrong off-primary).
    // Needs UIAccess: the ENABLED publish fails without it (rig-measured ERROR_ACCESS_DENIED;
    // the DISABLED call succeeds regardless - never probe availability with the disable shape).
    if (cfg.magInputTransform != 0 && (changed || ixPending_)) {
        // Decimation (issue #189): the publish exists for pointer-framework HOVER hit-testing
        // (clicks ride the welded cursor and never consult it), so it does not need the 144Hz
        // motion rate - every Nth changed tick suffices, with a GUARANTEED publish the moment
        // motion rests (changed goes false with one pending) so a stationary aim is always
        // exact. Halves-or-better the per-tick DWM magnification-message rate during ramps/pans
        // (this call postdates every hitch baseline and was fully uninstrumented until now).
        if (changed) ixPending_ = true;
        const bool rest = !changed;
        if (rest || ++ixTick_ >= cfg.ixDecimate) {
            ixTick_ = 0;
            ixPending_ = false;
            // srcL/srcT, not r.srcLeft/srcTop: when the ramp limiters make applyLevel != level
            // the VISUAL transform uses the recomputed origin, and the input mapping must
            // describe what is actually on screen.
            InputTransformRects ir = ComputeInputTransformRects(
                srcL, srcT, applyLevel, mon_.x, mon_.y, mon_.w, mon_.h);
            RECT dst{ ir.dl, ir.dt, ir.dr, ir.db };
            RECT src = (cfg.magInputTransform == 2) ? dst : RECT{ ir.sl, ir.st, ir.sr, ir.sb };
            LARGE_INTEGER fr, a, b;
            QueryPerformanceFrequency(&fr); QueryPerformanceCounter(&a);
            bool ok = host_.setInputTransform(applyLevel > 1.001, src, dst);
            QueryPerformanceCounter(&b);
            noteIxWrite(double(b.QuadPart - a.QuadPart) * 1000.0 / fr.QuadPart, ok);
            if (!ok) {
                // Self-heal (spec constraint 1): a failed ENABLED publish means this build
                // cannot fix the pointer-framework dead zones - the DESKTOP pick must stop
                // choosing the transform. Games are unaffected (legacy input surfaces).
                inputTransformAvailable_ = false;
                if (!inputXformWarned_) {
                    inputXformWarned_ = true;
                    wind::Log(wind::LogLevel::Warn, "transform",
                              "MagSetInputTransform failed (no UIAccess?) - desktop pick disabled");
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
    if (!ex.suppressCursorSync) {
        int cx = ex.clickOverride ? ex.clickDesktopX : (r.clickDesktopX + mon_.x);
        int cy = ex.clickOverride ? ex.clickDesktopY : (r.clickDesktopY + mon_.y);
        if (!haveLastClick_ || cx != lastClickX_ || cy != lastClickY_) {
            SetCursorPos(cx, cy);
            lastClickX_ = cx; lastClickY_ = cy; haveLastClick_ = true;
            weldedLastFrame_ = true;
        }
    }

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
            const int sx = spriteBand16_ ? (int)(r.cursorScreenX + 0.5) + mon_.x
                                         : r.clickDesktopX + mon_.x;
            const int sy = spriteBand16_ ? (int)(r.cursorScreenY + 0.5) + mon_.y
                                         : r.clickDesktopY + mon_.y;
            if (sx != lastSpriteX_ || sy != lastSpriteY_) {
                sprite_->moveTo(sx, sy);
                lastSpriteX_ = sx; lastSpriteY_ = sy;
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
    // MPO buster (issue #191): keep the ghost asserted while wanted (500ms cadence; assert_
    // also shows it on the first wanted tick). Hidden promptly when the session stops being
    // MPO-exposed (alt-tab to the desktop mid-zoom, knob turned off).
    if (mpoBusterWanted_ && level > 1.001) {
        unsigned long long nowG = GetTickCount64();
        if (nowG - lastGhostAssertMs_ >= 500) { lastGhostAssertMs_ = nowG; mpoGhost_.assert_(); }
    } else {
        mpoGhost_.hide();
    }
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
