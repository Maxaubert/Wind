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
    lastChangeMs_ = 0; keepAliveTick_ = 0; hiRampTick_ = 0;
    lastInputXformOn_ = false;
    lastSpriteX_ = INT_MIN; lastSpriteY_ = INT_MIN;
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
        sprite_->create(zorderBand_);   // above the shell so the cursor covers the magnified taskbar
    }
    if (smoothPan_) pin_.create();
    ready_ = true;
    return true;
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
        // (A sub-pixel "session warm-up" write here was tried and measured WORSE: 4 spike frames
        // per 3 cycles vs 2, and it added zoom-out spikes. Entering magnification costs ~36ms
        // once per zoom-in regardless - that is DWM building its machinery.)
        ensureMag();
        identityParked_ = false;
        idleSinceMs_ = 0;
        return;
    }
    if (!magUp_) return;
    // Give the real pointer back the moment the zoom ends (the follow-session sprite above hid
    // it). Done here, while the context is still alive - MagShowSystemCursor needs one.
    if (cursorHidden_) {
        if (sprite_) sprite_->hide();
        MagShowSystemCursor(TRUE);
        if (blanker_) blanker_->restore();
        cursorHidden_ = false;
    }
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
    weldedLastPresent_ = false;   // set true only if the deduped weld below fires (#174)
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
    if (level > sessionMaxLevel_) sessionMaxLevel_ = level;
    const bool ramping = applyLevel != level || (applyLevel != lastLevel_ && lastLevel_ > 0.0);
    MagTransform m = ComputeMagTransform(srcL, srcT, applyLevel, mon_.w, mon_.h);
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
    if (!changed && !ramping && applyLevel <= 8.0 &&
        GetTickCount64() - lastChangeMs_ < 700) {
        keepAliveTick_ ^= 1;
        txJitter = keepAliveTick_;
    }
    writeTransform((float)applyLevel, m.offX, m.offY, m.txX + txJitter, m.txY, fastPan_, false);
    // Input transform (issue #148): teach the input stack the inverse mapping. Win32 mouse input
    // is proven correct without it (instrumented), but pointer-stack apps (Explorer XAML lists,
    // Chromium content) hit-test through this - without it they get level/edge-dependent dead
    // zones. Needs UIAccess; on the dev build the call fails and is logged once.
    // A/B knob (hot): magInputTransform=1 publishes the input transform; 0 (default) leaves the
    // OS default. Unvalidated for mouse (docs scope it to pen/touch) - measured live by the user.
    if (changed && cfg.magInputTransform != 0) {
        RECT src{ (LONG)(r.srcLeft + 0.5), (LONG)(r.srcTop + 0.5),
                  (LONG)(r.srcLeft + mon_.w / applyLevel + 0.5),
                  (LONG)(r.srcTop + mon_.h / applyLevel + 0.5) };
        RECT dst{ 0, 0, mon_.w, mon_.h };
        bool ok = host_.setInputTransform(applyLevel > 1.001, src, dst);
        if (!ok && !inputXformWarned_) {
            inputXformWarned_ = true;
            wind::Log(wind::LogLevel::Warn, "transform",
                      "MagSetInputTransform failed (no UIAccess?)");
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
    {
        int cx = ex.clickOverride ? ex.clickDesktopX : (r.clickDesktopX + mon_.x);
        int cy = ex.clickOverride ? ex.clickDesktopY : (r.clickDesktopY + mon_.y);
        if (!haveLastClick_ || cx != lastClickX_ || cy != lastClickY_) {
            SetCursorPos(cx, cy);
            lastClickX_ = cx; lastClickY_ = cy; haveLastClick_ = true;
            weldedLastPresent_ = true;   // RunTick baselines the oracle at this point (#174)
        }
    }
    // (old note) FOLLOW design (issue #148 TDR root cause): the transform model NEVER places the OS cursor.
    // The per-tick SetCursorPos weld here was the driver killer: ANY programmatic ABSOLUTE cursor
    // placement (SetCursorPos or SendInput-absolute) while DWM fullscreen magnification is active
    // over a fullscreen game TDRs the NVIDIA driver within seconds - at any rate (20Hz died),
    // even with the transform parked static. Repro-proven with an 88-line UIAccess tool (zoom+pan
    // writes alone: 9.7k writes clean; add the weld: dead in 3.5s; lens-follows-read-cursor with
    // hand-equivalent relative input: clean). Native Magnifier never places the cursor - it
    // FOLLOWS it; so do we now: the real cursor stays visible (DWM displays it magnified, at
    // T(cursor) = screen center by the mapper's centered geometry), RunTick feeds the mapper the
    // READ cursor deltas 1:1, and hover + clicks are correct by construction (the displayed
    // cursor always sits over its own content). DO NOT reintroduce any SetCursorPos /
    // absolute-injection on this path, however tempting for alignment - it is the crash.

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
        sprite_->moveTo(r.clickDesktopX + mon_.x, r.clickDesktopY + mon_.y);
        sprite_->keepOnTop();
    } else if (useSprite_ && sprite_ && ex.gameFreeze && ex.drawCursor) {
        // GAME session (frozen cursor): the sprite is the aim point at the lens center. Desktop
        // coords; the transform displays it at the screen center. Deduped - the sprite parks at
        // the center except during edge slides, so most ticks issue no window move at all.
        CursorSprite::ShapeStatus st = sprite_->refreshShape();
        if (st == CursorSprite::ShapeStatus::Rendered) {
            int sx = r.clickDesktopX + mon_.x, sy = r.clickDesktopY + mon_.y;
            if (sx != lastSpriteX_ || sy != lastSpriteY_) {
                sprite_->moveTo(sx, sy);
                lastSpriteX_ = sx; lastSpriteY_ = sy;
            }
            sprite_->show();
            sprite_->keepOnTop();
        } else {
            sprite_->hide();   // Hidden/Unsupported shape: nothing sensible to draw
        }
    } else if (useSprite_ && sprite_ && ex.drawCursor && level > 1.001) {
        // The REAL cursor is welded to the lens point above, so input is entirely native - but
        // the hardware pointer is not magnified and is drawn at its raw desktop position, which
        // reads as a small cursor sitting away from the content it addresses. So hide it and
        // draw the marker at cursorScreen: the screen point where that content actually appears.
        // Composited outside the magnification, so it keeps a CONSTANT on-screen size at every
        // zoom level (the standing product rule).
        if (!cursorHidden_) {
            blanker_->blank();
            MagShowSystemCursor(FALSE);
            cursorHidden_ = true;
        }
        if (sprite_->refreshShape() == CursorSprite::ShapeStatus::Rendered) {
            // DESKTOP coords, not screen: DWM magnifies layered windows too, so the sprite must
            // live at the lens point in desktop space - the transform then displays it exactly
            // where that content appears. (Placing it in screen space put it off-screen once
            // transformed, which is why the pointer vanished at high zoom.) The consequence is
            // that the marker grows with the zoom, like the native Magnifier's pointer.
            const int sx = r.clickDesktopX + mon_.x;
            const int sy = r.clickDesktopY + mon_.y;
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
        // Desktop FOLLOW: the real cursor is visible and magnified by DWM (native Magnifier's own
        // look) - the sprite is only for the Inspect crosshair and the game-session aim point.
        sprite_->hide();
    }

    if (smoothPan_ && level > 1.0) {
        unsigned long long now = GetTickCount64();
        if (now - lastPinAssertMs_ >= 500) { lastPinAssertMs_ = now; pin_.assert_(); }
    } else {
        pin_.hide();
    }
}

void TransformModel::shutdown() {
    teardownMag();
    if (sprite_) sprite_->destroy();
    if (blanker_) blanker_->restore();
    pin_.destroy();
    ready_ = false;
}
}
