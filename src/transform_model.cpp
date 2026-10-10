#include "transform_model.h"
#include "transform.h"   // ComputeMagTransform
#include "tx_warm.h"     // WarmAction (pure, tested): the pan-start hitch fix
#include "logging.h"
#include "sprite_layer.h"  // PickSpriteLayer (pure, tested): issue #269
#include "config_path.h"   // ResolveLogDir
#include "tick_span.h"     // per-tick spans (#361)
#include "native_cursor.h" // NudgeAfterWrite, HoldInputPublish (pure, tested): issue #369
#include "dwm_watch.h"     // DwmGeneration: re-apply DWM-held state after a restart (#396)
#include "zoom_ladder.h"   // SnapSmoothLevel (pure, tested): issue #369
#include <cstdio>
#include <windows.h>
#include <magnification.h>
#include <cmath>
#include <atomic>

namespace wind {

// MagShowSystemCursor hides or restores the real pointer for Inspect and the hide-cursor hotkey.
// Thread-affine like the rest of the Magnification API: only the tick thread calls it.
static bool ShowSystemCursor(BOOL show) {
    wind::SpanScope span(wind::kSpanCursor);
    return MagShowSystemCursor(show) != FALSE;
}
// The pixel-and-back cursor event the native cursor relies on (issue #369). Never while a click is
// in progress: a nudge between button-down and button-up made some clicks fail to register (field
// report), so nothing is injected while the left, right or middle button is held or for 250 ms
// after it was last seen down. The side buttons are exempt: they are Wind's zoom keys.
// Shared by the tick thread and the cursor blanker's worker (its restore callback nudges), so the
// stamp is atomic and only ever moves forward.
static std::atomic<unsigned long long> g_lastButtonMs{0};
static std::atomic<bool> g_nudgeOwed{false};
static std::atomic<unsigned long long> g_heldSinceMs{0};   // first tick a button was seen down; 0 = up
static bool ClickInProgress() {
    const unsigned long long now = GetTickCount64();
    if ((GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_RBUTTON) | GetAsyncKeyState(VK_MBUTTON)) & 0x8000) {
        unsigned long long prev = g_lastButtonMs.load(std::memory_order_relaxed);
        while (prev < now && !g_lastButtonMs.compare_exchange_weak(prev, now, std::memory_order_relaxed)) {}
        unsigned long long none = 0;
        g_heldSinceMs.compare_exchange_strong(none, now, std::memory_order_relaxed);
    } else {
        g_heldSinceMs.store(0, std::memory_order_relaxed);
    }
    return WithinClickWindow(now, g_lastButtonMs.load(std::memory_order_relaxed));
}
static bool NudgeBlocked() {
    const bool click = ClickInProgress();
    return NudgeBlockedByClick(click, GetTickCount64(), g_heldSinceMs.load(std::memory_order_relaxed));
}
// A nudge skipped for a click is recorded as owed; DeliverOwedNudge sends it once the click window
// has ended (zoom can be bound to a mouse button, so the transition often lands inside the window
// and a still pointer would otherwise stay invisible until the hand moved). A press held past the
// window is a drag and is nudged as usual (NudgeBlockedByClick).
static void NudgePointer() {
    if (NudgeBlocked()) { g_nudgeOwed.store(true, std::memory_order_relaxed); return; }
    g_nudgeOwed.store(false, std::memory_order_relaxed);
    POINT np;
    if (GetCursorPos(&np)) { SetCursorPos(np.x + 1, np.y); SetCursorPos(np.x, np.y); }
}
static void DeliverOwedNudge() {
    if (!NudgeDue(g_nudgeOwed.load(std::memory_order_relaxed), NudgeBlocked())) return;
    NudgePointer();
}
void TransformModel::resetTransformState() {
    nativePrimed_ = false;  // a rebuilt context needs its own public prime (#369)
    forceWrite_ = false;    // lastLevel_ = 0 already forces the next write
    ixPubLevel_ = 0.0;
    ladderReq_ = 0.0; ladderOut_ = 0.0;
    // Everything the write path caches must be forgotten across a teardown, or the next session
    // compares against values DWM no longer holds and skips the writes that would re-apply them.
    lastLevel_ = 0.0; lastRequestedLevel_ = 0.0;
    lastOffX_ = lastOffY_ = lastTxX_ = lastTxY_ = 0;
    lastChangeMs_ = 0; lastWarmMs_ = 0; keepAliveTick_ = 0;
    ghostSessionStartMs_ = 0;
    lastInputXformOn_ = false;
    ixTick_ = 0; ixPending_ = false;
    haveLastClick_ = false;
    appliedSampling_ = -2;      // re-apply sampling mode on the next context (DWM-global state)
    sampleTryMode_ = -2;        // ...with a fresh set of attempts (#274)
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
    resetTransformState();
    return true;
}

void TransformModel::teardownMag() {
    edgeClipManage(false);   // never strand our clip across a teardown
    if (!magUp_) return;
    LARGE_INTEGER fr, a, b;
    QueryPerformanceFrequency(&fr); QueryPerformanceCounter(&a);
    // Cursor state FIRST: MagShowSystemCursor needs a live context, so undoing it after
    // MagUninitialize would silently fail and strand the pointer hidden.
    if (cursorHidden_) { ShowSystemCursor(TRUE); cursorHidden_ = false; }
    sprite_->hide();
    if (blanker_->blanked()) blanker_->restore();
    setDwmCentre(false);
    host_.setTransform(1.0f, 0, 0, 0, 0, false);   // leave DWM at identity before releasing
    RECT full{ 0, 0, mon_.w, mon_.h };
    host_.setInputTransform(false, full, full);
    host_.shutdown();                              // MagUninitialize: DWM leaves magnification mode
    mpoGhost_.hide();                              // never stranded shown across a teardown
    magUp_ = false;
    resetTransformState();
    if (active_) {
        // Released mid-session (shutdown, model swap): the pointer leaves DWM's composition for the
        // hardware plane, which Windows repaints only on the next cursor EVENT, so nudge it a pixel
        // and back (the same trick the zoom-out uses).
        NudgePointer();
    }
    lensFailed_ = false;      // a fresh context may build the lens
    QueryPerformanceCounter(&b);
    wind::Log(wind::LogLevel::Info, "transform", "magnification context released in %.1fms",
              double(b.QuadPart - a.QuadPart) * 1000.0 / fr.QuadPart);
}

// GetWindowBand is undocumented (user32 exports it since Windows 8), so it is resolved once at
// runtime; 0 = unknown, which PickSpriteLayer treats like an ordinary window.
static int QueryWindowBand(HWND h) {
    using PFN = BOOL(WINAPI*)(HWND, DWORD*);
    static PFN fn = reinterpret_cast<PFN>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetWindowBand"));
    DWORD band = 0;
    if (!h || !fn || !fn(h, &band)) return 0;
    return static_cast<int>(band);
}

// Issue #269. Cheap enough to run every tick: GetForegroundWindow, plus one GetWindowBand only
// when the foreground window changes (a window's band is fixed at creation).
void TransformModel::updateSpriteLayer() {
    if (!sprite_ || !sprite_->hasHigh()) return;
    HWND fg = GetForegroundWindow();
    if (fg != layerFg_) { layerFg_ = fg; layerFgBand_ = QueryWindowBand(fg); }
    const SpriteLayer want = PickSpriteLayer(cursorBandAuto_, true, layerFgBand_);
    if (want == sprite_->layer()) return;
    sprite_->setLayer(want);
    wind::Log(wind::LogLevel::Info, "transform", "cursor sprite -> %s (foreground band %d)",
              want == SpriteLayer::High ? "band 16" : "low band", layerFgBand_);
}

bool TransformModel::initialize(const MonitorTarget& monitor) {
    mon_ = monitor;
    // NO warm-up WRITE at launch (issue #148, field-measured): the first fullscreen-transform write
    // puts DWM into magnification-aware compositing, and from then on every cursor visibility or
    // shape change an app makes pays that path - a game that hides/shows the pointer on each
    // middle-click (Foundation: 25 visibility flips per test) then hitches while Wind merely RUNS
    // at 1x. Harness: 15 middle-click drags = 24 spike frames with the warm-up, 0 without. The
    // context and the cursor lens are built by idleTick (a context alone costs nothing, the lens
    // style stays OFF until a zoom starts); the first write waits for the first zoom.
    // The blanker and the sprite window serve Inspect and the hide-cursor hotkey: the sprite is
    // only ever the Inspect crosshair, the blanker hides the real pointer under it.
    blanker_ = std::make_unique<CursorBlanker>();
    sprite_  = std::make_unique<CursorSprite>();
    sprite_->create(zorderBand_, cursorBandAuto_);
    wind::Log(wind::LogLevel::Info, "transform", "crosshair sprite: band %d%s",
              sprite_->usedBand(),
              sprite_->hasHigh() ? ", auto-switching to band 16 (cursorBandAuto)"
                                 : (cursorBandAuto_ ? ", no band-16 twin (needs UIAccess)" : ""));
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
                inputTransformAvailable_ = uiAccessProbe_ = uiAccess != 0;
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
        if (ixMaxMs_ > 5.0 || ixFails_ > 0 || ixStomps_ > 0) {
            wind::Log(wind::LogLevel::Info, "ixwrite",
                      "publishes=%d avg=%.2fms MAX=%.1fms fails=%d stomps=%d",
                      ixCount_, ixCount_ ? ixSumMs_ / ixCount_ : 0.0, ixMaxMs_, ixFails_,
                      ixStomps_);
        }
        ixLogMs_ = now; ixMaxMs_ = 0.0; ixSumMs_ = 0.0; ixCount_ = 0; ixFails_ = 0;
        ixStomps_ = 0;
    }
}

void TransformModel::noteIxStomp() {
    std::lock_guard<std::mutex> lk(statMx_);
    ++ixStomps_;
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
// re-scale work, addressed by txMaxStepPct. Writes stay on the tick thread; only the timing
// instrumentation remains.
void TransformModel::writeTransform(float lvl, int offX, int offY, int tx, int ty, bool fast) {
    LARGE_INTEGER fr, a, b;
    QueryPerformanceFrequency(&fr); QueryPerformanceCounter(&a);
    bool ok = host_.setTransform(lvl, offX, offY, tx, ty, fast);
    QueryPerformanceCounter(&b);
    noteWrite(double(b.QuadPart - a.QuadPart) * 1000.0 / fr.QuadPart, ok);
}

// DWM centring switch (issue #369). Turning it on hands the pan to DWM; turning it off hands it
// back. Either way the caller forces the next transform write: after TRUE, DWM keeps the factor of
// the next write (it would otherwise centre with a stale one), and after FALSE the view must be
// put back on Wind's own offset.
void TransformModel::setDwmCentre(bool on) {
    if (on == dwmCentreOn_) return;
    if (on && dwmCentreBroken_) return;
    const bool ok = host_.setDwmCentring(on);
    if (on && !ok) {
        dwmCentreBroken_ = true;
        wind::Log(wind::LogLevel::Warn, "transform",
                  "DWM centring unavailable (SetFullscreenMagnifierOffsetsDWMUpdated) - Wind keeps the pan");
        return;
    }
    dwmCentreOn_ = on;
}

void TransformModel::hideSystemCursor(bool hide) {
    if (hide && !ensureMag()) return;   // MagShowSystemCursor needs a live context
    cursorHidden_ = hide;
    // During NORMAL zoom the real pointer stays visible (DWM draws it magnified) and main.cpp never
    // calls this. It is called only for INSPECT sessions, where the frozen real pointer must vanish
    // under the crosshair: blanker for standard cursors, MagShowSystemCursor for the plane
    // wholesale (app-custom cursors). Both are undone on exit.
    if (hide) { blanker_->blank(); ShowSystemCursor(FALSE); }
    else      { sprite_->hide(); ShowSystemCursor(TRUE); blanker_->restore(); }
}


// Session-scoped edge clip (cfg.edgeClip; the full story is in config.h). Keeps the pointer off
// the outermost pixel ring so the WM_SETCURSOR shape war there can never start. Rules, each one
// load-bearing:
//   - SNAPSHOT the existing clip on engage and RESTORE it on release: this rig runs a permanent
//     external work-area clip, and releasing to nullptr would destroy it.
//   - INTERSECT with the existing clip rather than replace it.
//   - NEVER fight a tighter clip: a game confining the pointer, or Inspect's 1px freeze, already
//     keeps the pointer off the edge - adopt, do not overwrite.
//   - Re-assert only when a foreign write WIDENED the clip back over the edge pixels (deduped by
//     comparing against what we applied), so there is no per-tick churn.
void TransformModel::edgeClipManage(bool wantActive) {
    RECT cur{};
    if (!GetClipCursor(&cur)) return;
    const RECT inset{ mon_.x + 1, mon_.y + 1, mon_.x + mon_.w - 1, mon_.y + mon_.h - 1 };
    if (!wantActive) {
        if (edgeClipActive_) {
            // Restore the snapshot unless someone else took the clip meanwhile (theirs wins).
            if (EqualRect(&cur, &edgeClipApplied_)) ClipCursor(&edgeClipSaved_);
            edgeClipActive_ = false;
        }
        return;
    }
    const bool coversEdge = cur.left < inset.left || cur.top < inset.top ||
                            cur.right > inset.right || cur.bottom > inset.bottom;
    if (!edgeClipActive_) {
        if (!coversEdge) return;                     // something tighter already owns the pointer
        edgeClipSaved_ = cur;
        RECT want{};
        if (!IntersectRect(&want, &cur, &inset)) return;
        ClipCursor(&want);
        edgeClipApplied_ = want;
        edgeClipActive_ = true;
        return;
    }
    // Active: re-assert only if a foreign write re-opened the edge pixels.
    if (!EqualRect(&cur, &edgeClipApplied_)) {
        if (!coversEdge) {                            // foreign but tighter (game/Inspect): adopt
            edgeClipSaved_ = cur;                     // restoring THEIR clip at session end is
            edgeClipApplied_ = cur;                   // wrong; they own it now - track and yield
            edgeClipActive_ = false;
            return;
        }
        edgeClipSaved_ = cur;                        // foreign and wide: re-snapshot, re-inset
        RECT want{};
        if (!IntersectRect(&want, &cur, &inset)) return;
        ClipCursor(&want);
        edgeClipApplied_ = want;
    }
}

void TransformModel::setActive(bool active) {
    active_ = active;
    if (!active && !inputXformDenied_) inputTransformAvailable_ = uiAccessProbe_;   // re-arm a transient failure
    if (active) {
        LARGE_INTEGER zf, z1, z2;   // zoom timeline split (#310): a few QPC reads, always on
        QueryPerformanceFrequency(&zf);
        lastEnter_.wasWarm = magUp_;
        // Native cursor (issue #369): DWM draws the real pointer, so there is nothing to stand
        // up and nothing to blank - no cursor swaps at zoom-in at all.
        QueryPerformanceCounter(&z1);
        // (A sub-pixel "session warm-up" write here was tried and measured WORSE: 4 spike frames
        // per 3 cycles vs 2, and it added zoom-out spikes. Entering magnification costs ~36ms
        // once per zoom-in regardless - that is DWM building its machinery.)
        ensureMag();
        // Native cursor: switch the pointer to DWM's composition (0.2 ms once the lens exists; a
        // first zoom before the idle warm-up pays the lens build here, once). win32k sends the
        // new cursor mode to DWM only on the next pointer update (move or shape), so without a
        // cursor event the small hardware pointer stays on screen for the whole zoom-in while the
        // hand is still (measured: no composed pointer in any ramp frame). Nudge a pixel and back.
        if (host_.createCursorLens()) {
            host_.setCursorLens(true);
            NudgePointer();
        }
        QueryPerformanceCounter(&z2);
        lastEnter_.ensureMagMs = double(z2.QuadPart - z1.QuadPart) * 1000.0 / zf.QuadPart;
        return;
    }
    if (!magUp_) return;
    // Teardown breakdown (#361): every step of the zoom-out timed and logged as one line, so a
    // long zoom-out names its step instead of being one opaque total.
    LARGE_INTEGER tf, t0; QueryPerformanceFrequency(&tf); QueryPerformanceCounter(&t0);
    LARGE_INTEGER tPrev = t0;
    double stepMs[9] = {};
    auto step = [&](int i) {
        LARGE_INTEGER n; QueryPerformanceCounter(&n);
        stepMs[i] = double(n.QuadPart - tPrev.QuadPart) * 1000.0 / double(tf.QuadPart);
        tPrev = n;
    };
    // MPO buster: hide strictly AFTER the identity park below would be wrong - the park writes
    // identity while the game may still be mid-demotion-return; hiding HERE (before the park)
    // is also wrong for the same reason in reverse. Order chosen: park first (identity is a
    // safe value at any plane state), then hide the ghost - the game re-promotes to its plane
    // against a parked-identity transform, never against a live translation.
    // Give the real pointer back the moment the zoom ends (Inspect or the hide-cursor hotkey hid
    // it). Done here, while the context is still alive - MagShowSystemCursor needs one.
    if (cursorHidden_) {
        sprite_->hide();
        ShowSystemCursor(TRUE);
        cursorHidden_ = false;
    }
    step(0);   // crosshair hide + system cursor show
    setDwmCentre(false);   // before the identity park, so DWM does not re-centre against it
    // Only a session that blanked the cursor set (Inspect, the hide-cursor hotkey) has anything to
    // give back; the rest never touched it, so there is no scheme reload.
    if (blanker_->blanked()) {
        // Windows repaints the pointer plane only on the next cursor EVENT, so a restored-but-
        // still pointer stays invisible until the hand moves (field-verified). A 1px nudge and
        // back generates that event invisibly. It must FOLLOW the restore, which runs on the
        // blanker's worker (#363: the scheme reload froze the 1x landing frame for 8-90 ms), so
        // the nudge rides along on the worker too.
        blanker_->restore([] {
            NudgePointer();
        });
        step(1);   // system cursor restore queued (it was the whole teardown cost)
    }
    step(2);   // pointer nudge
    edgeClipManage(false);             // give the clip back before the session winds down
    step(3);
    const double endMaxLevel = sessionMaxLevel_;
    if (traceOn_) traceDump();
    sessionMaxLevel_ = 0.0;
    step(4);   // trace hand-off
    // Park at EXACT identity right here, at the end of the zoom-out. Returning DWM to identity
    // costs a ~150ms compositor stall no matter when it happens (measured), so pay it while the
    // user is still in zoom motion and expects movement - not 1.2s later while they are playing.
    LARGE_INTEGER fr, pa, pb;
    QueryPerformanceFrequency(&fr); QueryPerformanceCounter(&pa);
    // restLevel_ > 1.0 parks a hair off identity so DWM stays in magnification mode across the
    // idle - see cfg.txRestLevel for why and what it costs. 1.0 is the shipped behaviour.
    host_.setTransform((float)restLevel_, 0, 0, 0, 0, false);
    QueryPerformanceCounter(&pb);
    step(5);   // identity park
    // The park applied 1.0 outside writeTransform, so sync the cached level: a stale lastLevel_
    // here anchored the step cap's next session at the trailing zoom-out value (#219 bounce).
    lastLevel_ = restLevel_; lastRequestedLevel_ = restLevel_;
    const double parkMs = double(pb.QuadPart - pa.QuadPart) * 1000.0 / fr.QuadPart;
    if (parkMs > 5.0)
        wind::Log(wind::LogLevel::Info, "transform", "identity park took %.1fms", parkMs);
    if (host_.cursorLensReady()) {
        // Native cursor: back to the hardware pointer at 1x (the composed one taxes every cursor
        // change a game makes). The hardware plane repaints only on the next cursor EVENT, so
        // nudge the pointer a pixel and back.
        host_.setCursorLens(false);
        NudgePointer();
    }
    RECT full{ 0, 0, mon_.w, mon_.h };
    host_.setInputTransform(false, full, full);   // input mapping back to identity at 1x
    // The context outlives the session (idleTick keeps it), so resetTransformState does not run
    // between sessions: forget the last publish level here, or a quick zoom back to the same level
    // compares equal and skips the nudge that makes DWM draw the pointer again.
    ixPubLevel_ = 0.0;
    step(6);
    // Stomp-guard expectation (issue #217): the slot should now read DISABLED. Kept valid across
    // the idle so the next session's first tick catches a rect stranded meanwhile (e.g. a native
    // Magnifier killed while zoomed) and overwrites it immediately.
    ixExpectedValid_ = true;
    ixExpectedOn_ = false;
    pin_.hide();
    step(7);
    mpoGhost_.hide();   // after the identity park: re-promotion happens against a parked value
    step(8);
    const double total = double(tPrev.QuadPart - t0.QuadPart) * 1000.0 / double(tf.QuadPart);
    wind::Log(wind::LogLevel::Info, "txsession",
              "session end maxLevel=%.2f teardown=%.2fms (cursorShow=%.2f blankerRestore=%.2f nudge=%.2f "
              "clip=%.2f trace=%.2f park=%.2f ix=%.2f pin=%.2f ghost=%.2f)",
              endMaxLevel, total, stepMs[0], stepMs[1], stepMs[2], stepMs[3], stepMs[4], stepMs[5],
              stepMs[6], stepMs[7], stepMs[8]);
}

void TransformModel::idleTick() {
    DeliverOwedNudge();   // a nudge a click skipped at the zoom-out is still owed at 1x
    // NATIVE CURSOR (issue #369): keep the context and the cursor lens alive at 1x instead of
    // releasing them. With the lens style OFF this costs nothing (measured: a pointer-toggling
    // full-screen app keeps Independent Flip, 0 spike frames), and it moves the one-time 60-125 ms
    // lens build off the zoom path: it runs here, at 1x, shortly after launch. The old release
    // existed for the cursor-change tax, which only the composed pointer (style ON) causes.
    if (active_) return;
    if ((!magUp_ || !host_.cursorLensReady()) && !lensFailed_) {
        LARGE_INTEGER fr, a, b;
        QueryPerformanceFrequency(&fr); QueryPerformanceCounter(&a);
        const bool ok = ensureMag() && host_.createCursorLens();
        QueryPerformanceCounter(&b);
        lensFailed_ = !ok;   // no 100 ms retry loop; zoom-in falls back to the public prime
        if (!lensLogged_) {
            lensLogged_ = true;
            wind::Log(ok ? wind::LogLevel::Info : wind::LogLevel::Warn, "transform",
                      "cursor lens %s in %.1fms (kept warm at 1x)", ok ? "ready" : "FAILED",
                      double(b.QuadPart - a.QuadPart) * 1000.0 / fr.QuadPart);
        }
    }
}

void TransformModel::present(const MapResult& r, double level, const Config& cfg,
                             const MonitorTarget& mon, const PresentExtras& ex) {
    (void)mon;
    // CENTERED-CURSOR geometry (issue #148 revival). Wind's identity is the centred cursor, and the
    // render model's mapper already solves the whole centered geometry - including the edge zones,
    // where the pointer slides away from the centre as the source rect clamps. So the mapper's
    // CENTERED source rect is the transform, and DWM draws the real pointer into the magnified frame
    // itself (native cursor, issue #369): nothing here places a pointer of our own.
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
    const bool rampStopped = (level == lastRequestedLevel_);   // the controller stopped requesting new levels
    const double prevRequestedLevel = lastRequestedLevel_;
    lastRequestedLevel_ = level;
    // txMaxStepPct: rate-limit the APPLIED level change per tick. Each change makes DWM re-scale
    // its cached surfaces and that cost grows with the level, so an unclamped fast ramp demands
    // the most expensive re-scales back to back exactly at the top - measured (#219): ~15% of
    // uncapped 15x zoom-ins stalled 35-43ms then snapped 1.2-1.9 levels; capped, 20/20 ramps
    // ran even. UP-steps ONLY: zoom-out measured clean uncapped (outGaps <=8ms in every soak),
    // and a DOWN clamp anchored on lastLevel_ is what caused the session-start BOUNCE (rig-
    // reproduced 4/4: 5-7 backward level steps at the start of a quick re-zoom) - the zoom-out
    // trailed the controller, the identity park bypassed lastLevel_, and the next ramp's first
    // writes were dragged back down toward the stale anchor. The applied level trails a fast
    // ramp and catches up within a few ticks of it stopping; the source rect below is recomputed
    // for whatever we apply.
    if (cfg.txMaxStepPct > 0 && lastLevel_ > 1.0 && applyLevel > lastLevel_) {
        const double up = lastLevel_ * (1.0 + cfg.txMaxStepPct / 1000.0);
        if (applyLevel > up) applyLevel = up;
    }
    // SMOOTH-ZOOM LADDER (issue #369, src/zoom_ladder.h): with smooth sampling DWM rounds its scratch
    // image's size and origin every frame, which shakes a continuous zoom; snap to the nearest level
    // whose predicted rounding error is under 1 px (never backwards in the ramp). Once the zoom has
    // settled the chosen level is held: panning at a fixed level does not shake, re-snapping would.
    // The ladder's held level differs from the requested one on purpose, so "still ramping" must be
    // judged on the level BEFORE the snap: comparing the snapped level with `level` read as a ramp
    // that never ended, which held the input-transform publish forever (hover dead zones).
    const double preLadderLevel = applyLevel;
    if (cfg.txSmoothLadder != 0 && cfg.txSamplingMode == 1 && applyLevel > 1.001) {
        if (rampStopped && applyLevel == level && level >= cfg.maxLevel - 1e-6) {
            // Stopped at the maximum: land on it exactly. A level at rest does not shake, and the
            // coarser steps above (RampStepHeld) could otherwise leave a clean level just under it.
            ladderReq_ = level;
            ladderOut_ = level;
        } else if (applyLevel == level && level == ladderReq_ && ladderOut_ > 0.0) {
            applyLevel = ladderOut_;
        } else if (lastLevel_ > 1.001 && std::fabs(applyLevel - lastLevel_) <= applyLevel * 1e-9) {
            // The request IS the level on screen (RunTick stopped the ease-out there): never re-snap
            // it, or the zoom jumps to a neighbouring clean level after it stopped (#375).
            ladderReq_ = level;
            ladderOut_ = lastLevel_;
            applyLevel = lastLevel_;
        } else if (rampStopped && !rampStepHeld_ && applyLevel == level && lastLevel_ > 1.001 &&
                   std::fabs(lastLevel_ - level) <= level * (SnapWindow(level) + 1e-5)) {
            // The zoom just stopped: keep the level already on screen rather than re-snapping, so
            // releasing the key never nudges the zoom in or out (field 2026-10-07).
            ladderReq_ = level;
            ladderOut_ = lastLevel_;
            applyLevel = lastLevel_;
        } else {
            // Direction from the requests, not from the level on screen (LadderDir, #429).
            const int dir = LadderDir(level, prevRequestedLevel, lastLevel_);
            if (LadderHoldsLevel(dir, applyLevel, lastLevel_)) {
                ladderReq_ = level;
                ladderOut_ = lastLevel_;
                applyLevel = lastLevel_;
            } else {
                const double snapped = SnapSmoothLevel(applyLevel, r.centerX, r.centerY, mon_.w, mon_.h,
                                                       lastLevel_ > 1.0 ? lastLevel_ : 0.0, dir);
                ladderReq_ = level;
                ladderOut_ = snapped;
                applyLevel = snapped;
            }
        }
    }
    // Smooth high-zoom steps (#429, RampStepHeld): while DWM centres, step the level less often.
    // A stopped request after a held step lands exactly, not on the ladder's "keep what is shown".
    rampStepHeld_ = cfg.txSamplingMode == 1 && dwmCentreOn_ &&
                    RampStepHeld(applyLevel, lastLevel_, cfg.txRampMinStep / 1000.0, cfg.txRampMinFrom, rampStopped);
    if (rampStepHeld_) applyLevel = lastLevel_;
    double srcL = r.srcLeft, srcT = r.srcTop;
    if (applyLevel != level) {
        OffsetF o = ComputeOffsetF(r.centerX, r.centerY, applyLevel, mon_.w, mon_.h);
        srcL = o.x; srcT = o.y;
    }
    ClickInProgress();   // keep the click window current between nudges
    DeliverOwedNudge();
    restLevel_ = cfg.txRestLevel;           // hot
    if (!ensureMag()) return;   // lazy context: the session's first write brings DWM up
    // Bitmap smoothing (issue #197/#227), once per magnification context. The smooth filter
    // is the WHOLE quality gap to native Magnifier: sharp magnified image and a cursor that
    // grows naturally with the zoom instead of pixelating. Known trade, accepted after a full
    // field investigation (2026-08-22): during LEVEL ramps the filter re-interpolates every
    // edge per scale step - a slight shimmer that nearest does not have. It is NOT our
    // geometry (written transforms proved sub-0.1px consistent), not write cadence, not
    // frame coherence, not the pointer plane - and native Magnifier shows the same artifact
    // character under its notchy ease. A nearest-during-ramp hotswap was field-tried and
    // rejected: the filters disagree about sub-pixel phase, so every swap shifted the image
    // 1-2px even with the source snapped to integers. One static filter, user's choice:
    // txSamplingMode 0 (DEFAULT, nearest: shimmer-free ramps) or 1 (EXPERIMENTAL smooth).
    // The flag is DWM-global and dies with a DWM restart, hence per-context re-apply.
    // A failed apply is retried, a little (issue #274): the call used to be recorded as applied
    // before it ran, so a wrong-thread failure left the filter unapplied for the whole context.
    // BOUNDED, so a failing setter can never re-issue every tick. (The "FALSE on every call" seen
    // in this rig's logs, 245 of 245, was the setter never being found: it was resolved by a
    // non-existent ordinal until #369.) Up to 3 attempts, 1 s apart, then
    // accept. The mode is DWM-global state, so a genuine miss is also re-tried per context.
    // A restarted dwm.exe starts nearest whatever win32k says (#396): forget what was applied.
    const unsigned long dwmGen = DwmGeneration();
    if (dwmGen != dwmGenSeen_) {
        dwmGenSeen_ = dwmGen;
        appliedSampling_ = -2;
        sampleTryMode_ = -2;
    }
    const int wantSampling = cfg.txSamplingMode;
    if (wantSampling >= 0 && appliedSampling_ != wantSampling) {
        const unsigned long long now = GetTickCount64();
        if (sampleTryMode_ != wantSampling) { sampleTryMode_ = wantSampling; sampleTries_ = 0; }
        if (sampleTries_ == 0 || now - sampleLastTryMs_ >= 1000) {
            const bool ok = host_.setSamplingMode((unsigned)wantSampling);
            ++sampleTries_;
            sampleLastTryMs_ = now;
            if (ok || sampleTries_ >= 3) {
                wind::Log(wind::LogLevel::Info, "transform", "bitmap smoothing %d applied=%d (tries %d)",
                          wantSampling, ok ? 1 : 0, sampleTries_);
                appliedSampling_ = wantSampling;
            }
        }
    }
    traceOn_ = cfg.txTrace != 0;
    if (level > sessionMaxLevel_) sessionMaxLevel_ = level;
    const bool ramping = preLadderLevel != level || (applyLevel != lastLevel_ && lastLevel_ > 0.0);
    // Edge sampling margin (see transform.h) applied to the SOURCE, not just to the written
    // transform: srcL/srcT go on to feed the input-transform publish below, and a visual rect
    // that sat one texel inside a published rect that did not would put the pointer framework's
    // hover hit-test one source pixel off along that edge - level px on screen. One rect.
    const EdgeMargins margins = EdgeMarginsFor(cfg.txSamplingMode, cfg.txEdgeMargin);
    {
        const double loX = SrcEdgeFloor(margins.lo, applyLevel, mon_.w);
        const double loY = SrcEdgeFloor(margins.lo, applyLevel, mon_.h);
        if (srcL < loX) srcL = loX;
        if (srcT < loY) srcT = loY;
    }
    MagTransform m = ComputeMagTransform(srcL, srcT, applyLevel, mon_.w, mon_.h,
                                         margins.lo, margins.hi);
    // 2D write-site 16-bit backstop (issue #191): when the session is MPO-exposed AND the ghost
    // is not verifiably holding the game off its overlay plane, the never-exceed-32767 invariant
    // is enforced HERE, structurally, regardless of the mapper walls (which divide by the
    // CONTROLLER level while this write uses the ramp-limited applyLevel - step-limit drift
    // otherwise spends the headroom on faith). Checked fresh at write time - a ghost that died
    // mid-session re-arms the clamp on the very next write, one tick before the wall re-engages.
    // Both channels recomputed so offsets and translations describe the same rect. MUST share the
    // wall's evidence gate: clamping while the walls are lifted would pin the view at the 32000
    // line while the mapper (and welded cursor) pan on past it.
    if (mpoExposed_ && (cfg.txSamplingMode == 0 || !mpoGhost_.settled(GetTickCount64()))) {
        // Nearest sampling never lifts the clamp (issue #242): the 16-bit field lives in the
        // nearest magnification path itself, ghost or no ghost - field-proven 2026-08-29.
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
    bool txWroteThisTick = false;   // for the trace
    // DWM CENTRING (issue #369, src/native_cursor.h). In a native-cursor session where the view is
    // a pure function of the pointer (RunTick's ex.dwmCentre), DWM re-centres the view itself on
    // every cursor update, latched with the pointer it draws: no tick-to-frame drift at any speed.
    // Switching off is allowed on a paused tick (it stops DWM moving the view); switching on waits
    // for a tick that may write, because the switch needs the forced write that follows it.
    // The owed write survives paused ticks (forceWrite_), so a switch-off during a pause still puts
    // Wind's offset back on the first tick that may write.
    {
        // A broken export (dwmCentreBroken_) removes the wish, so the switch is not retried (and the
        // write forced, the line logged) on every tick; the failure itself is logged once, in
        // setDwmCentre.
        const bool want = ex.dwmCentre && applyLevel > 1.001;
        if (WantDwmCentreSwitch(want, dwmCentreOn_, dwmCentreBroken_, ex.pauseWrites)) {
            const bool before = dwmCentreOn_;
            setDwmCentre(want);
            if (dwmCentreOn_ != before) {
                forceWrite_ = true;
                wind::Log(wind::LogLevel::Info, "transform", "DWM centring %s", dwmCentreOn_ ? "ON" : "off");
            }
        }
    }
    // Trace inputs, captured here and appended at the END of present().
    bool trChanged = false, trRamping = ramping, trWarm = false;
    const double trLevel = applyLevel;
    const int trTxX = m.txX, trOffX = m.offX;
    // pauseWrites (issue #148): a click's injected cursor move is in flight - a transform write
    // racing a cursor-position update is the proven TDR, so those ticks write NOTHING. State is
    // untouched; the next unpaused tick lands the same values. (The native cursor's pixel-and-back
    // nudges are the intentional exception to "no cursor events beside a write": they run right
    // AFTER the write on the same thread, never racing it, and never during a click.)
    if (!ex.pauseWrites) {
    const unsigned long long nowMs = GetTickCount64();
    const bool changed = m.offX != lastOffX_ || m.offY != lastOffY_ ||
                         m.txX != lastTxX_ || m.txY != lastTxY_ || applyLevel != lastLevel_;

    // EVERY changed tick is written. Issue #204 traced native Magnifier writing ~half as often and
    // tried to coalesce ours (a write-rate cap and a minimum pan step); both were field-rejected the
    // same day (the step reads as wobble under a slow hand, the cap as low fps at high zoom) and
    // are gone. The level is written straight, per tick, for the same reason.
    const bool levelMoved = applyLevel != lastLevel_;
    const bool forceWrite = forceWrite_;
    // A pan write while DWM centres must be followed by its nudge, which a click in progress
    // forbids: hold it until the click window ends (HoldWriteForClick, issue #381).
    const bool clickHold = HoldWriteForClick(dwmCentreOn_, ClickInProgress(), levelMoved, forceWrite);
    const bool writeNow = (changed || forceWrite) && !clickHold;

    if (writeNow) {
        lastOffX_ = m.offX; lastOffY_ = m.offY; lastTxX_ = m.txX; lastTxY_ = m.txY;
        lastLevel_ = applyLevel;
        lastChangeMs_ = nowMs;
        keepAliveTick_ = 0;
    }
    // Everything below keys off whether the write ACTUALLY goes out this tick, not merely whether
    // the values differ - the cached last* state must never claim a write we suppressed.
    const bool changedAndWriting = writeNow;
    txWroteThisTick = writeNow;
    trChanged = changed;
    int txJitter = 0;
    bool keepAliveActive = false;
    // WARM-KEEPING (see src/tx_warm.h for the measurements). At rest Wind would otherwise stop
    // writing entirely and DWM's composition falls back to the game's present rate, so the first
    // movement after a pause lands late - the hitch felt at every direction reversal. Only views
    // Wind writes itself need it (locked, Inspect, a detached view): DWM's own centring moves
    // the view on every cursor update.
    TxWarmIn wi;
    wi.wroteThisTick      = changedAndWriting;
    wi.ramping            = ramping;
    wi.mode               = cfg.txWarmMode;
    wi.allowed            = ex.warmAllowed;
    wi.applyLevel         = applyLevel;
    // Cadence (issue #246): the period counts from whichever came last, the previous pulse
    // closing or a real write (a real write resets keepAliveTick_ above, so no pulse is open).
    wi.warmHz             = cfg.txWarmHz;
    wi.pulseOpen          = keepAliveTick_ != 0;
    wi.sinceLastWarmMs    = nowMs - (lastWarmMs_ > lastChangeMs_ ? lastWarmMs_ : lastChangeMs_);
    // No warm pulses while DWM centres: each one would put Wind's offset back on screen for a
    // frame, and DWM's own per-cursor-update moves are the pan (native has no warm-keeping).
    if ((dwmCentreOn_ ? TxWarm::None : WarmAction(wi)) == TxWarm::Jitter1px) {
        keepAliveTick_ ^= 1;
        txJitter = keepAliveTick_;   // BOTH parities must write (the return-to-true half too)
        keepAliveActive = true;
        trWarm = true;
        if (keepAliveTick_ == 0) lastWarmMs_ = nowMs;   // pulse closed: the period starts here
    }

    // Same-value hygiene (issue #189): a zoomed-idle tick with nothing to write and no pulse due
    // would push an identical write 144x/s. DWM parks on static values anyway (measured), so
    // skipping is free; the next changed/keep-alive tick writes as before.
    if (changedAndWriting || keepAliveActive) {
        // Native cursor (#369): the context's first zoomed write goes through the PUBLIC API, which
        // is what makes DWM draw the real pointer magnified; the private channel keeps it after.
        // (Fallback only: normally the cursor lens does this without the 200-260 ms public write.)
        const bool prime = !nativePrimed_ && !host_.cursorLensReady() && applyLevel > 1.001;
        // The pulse also has to reach the PUBLIC channel (prime write, or after a private-channel
        // failure), which ignores the translation and reads the source offset: shift offX there by
        // 1 px, inward from the floor so the 1-texel left clamp and 2 px right clamp still hold.
        const int offJitter = (m.offX >= 2) ? -txJitter : txJitter;
        writeTransform((float)applyLevel, m.offX + offJitter, m.offY, m.txX + txJitter, m.txY, fastPan_ && !prime);
        if (prime) nativePrimed_ = true;
        if (NudgeAfterWrite(dwmCentreOn_, true)) {
            NudgePointer();
        }
        forceWrite_ = false;
    }
    // Input transform. Mode 1 (THE SHIPPED DEFAULT; field-verified 4x-20x,
    // POINTER-HITTEST-FINDINGS.md): publish the visual source rect on every change, exactly
    // like native Magnifier. Pointer-framework apps (Explorer/Settings/shell) hit-test mouse
    // input through this; without it the welded cursor has hard hover dead zones. Mode 2 =
    // enabled identity (diagnostic; measured DEAD). Mode 0 = off (diagnostic; measured DEAD).
    // Both rects in VIRTUAL-SCREEN coordinates (the old 0,0-based dst was wrong off-primary).
    // Needs UIAccess: the ENABLED publish fails without it (rig-measured ERROR_ACCESS_DENIED;
    // the DISABLED call succeeds regardless - never probe availability with the disable shape).
    // Stomp guard (issue #217, docs/NATIVE-MAGNIFIER-STOMP.md): the input transform is ONE
    // system-wide slot, and native Magnifier re-publishes an ENABLED IDENTITY into it
    // continuously while it runs - even sitting unzoomed at 100%. Under a Wind zoom that
    // identity mapping unmoors the visible cursor (the wobble); a dirty Magnifier exit strands
    // its last rect the same way. So every zoomed tick reads the slot back (~0.1ms) and, when
    // it does not hold what we last published, forces a republish past the decimation. Wind at
    // tick rate wins the two-writer war for as long as the foreign writer lives, and a stale
    // corpse is overwritten on the first tick of the next session.
    bool ixForce = false;
    if (cfg.magInputTransform == 1 && applyLevel > 1.001 && ixExpectedValid_) {
        bool aOn = false; RECT aSrc{}, aDst{};
        if (host_.getInputTransform(aOn, aSrc, aDst) &&
            InputTransformStomped(ixExpectedOn_, ixExpL_, ixExpT_, ixExpR_, ixExpB_,
                                  aOn, aSrc.left, aSrc.top, aSrc.right, aSrc.bottom)) {
            ixForce = true;
            noteIxStomp();
            // The same foreign writer owns the shared cursor-visibility global
            // (MagShowSystemCursor) - re-assert our hide on the stomp tick so the raw pointer
            // plane it re-showed does not stay visible under the Inspect crosshair.
            if (cursorHidden_) ShowSystemCursor(FALSE);
            if (!ixStompWarned_) {
                ixStompWarned_ = true;
                wind::Log(wind::LogLevel::Warn, "transform",
                          "foreign input-transform writer detected (native Magnifier running?) - "
                          "republishing per tick");
            }
        }
    }
    if (cfg.magInputTransform != 0 && (changed || ixPending_ || ixForce)) {
        // Decimation (issue #189): the publish exists for pointer-framework HOVER hit-testing
        // (clicks ride the welded cursor and never consult it), so it does not need the 144Hz
        // motion rate - every Nth changed tick suffices, with a GUARANTEED publish the moment
        // motion rests (changed goes false with one pending) so a stationary aim is always
        // exact. Halves-or-better the per-tick DWM magnification-message rate during ramps/pans
        // (this call postdates every hitch baseline and was fully uninstrumented until now).
        // A stomp bypasses the decimation entirely: correctness of the mapping beats hygiene.
        if (changed) ixPending_ = true;
        const bool rest = !changed;
        // Native cursor (#369): no publish while the level ramps (it hides the composed pointer);
        // the pending flag carries it to the first settled tick.
        // Held only while a zoom key/button drives the ramp: during the release ease-out hover must
        // follow (field 2026-10-07: tab hover waited for the whole 300 ms glide to end).
        const bool hold = HoldInputPublish(ramping && ex.zoomDriven, ixForce);
        if (!hold && (ixForce || rest || ++ixTick_ >= cfg.ixDecimate)) {
            ixTick_ = 0;
            ixPending_ = false;
            // The rect actually WRITTEN (m), not r.srcLeft/srcTop or srcL/srcT: the ramp
            // limiters, the far-edge floor in ComputeMagTransform and the 16-bit backstop all move
            // the visual origin after srcL/srcT, and the input mapping must describe what is on
            // screen. Publishing the unclamped source put hover hit-testing up to a source px
            // (level px on screen) off along the right/bottom edge (review of #394).
            InputTransformRects ir = ComputeInputTransformRects(
                (double)m.offX, (double)m.offY, applyLevel, mon_.x, mon_.y, mon_.w, mon_.h);
            RECT dst{ ir.dl, ir.dt, ir.dr, ir.db };
            RECT src = (cfg.magInputTransform == 2) ? dst : RECT{ ir.sl, ir.st, ir.sr, ir.sb };
            const bool enable = applyLevel > 1.001;
            LARGE_INTEGER fr, a, b;
            QueryPerformanceFrequency(&fr); QueryPerformanceCounter(&a);
            bool ok = host_.setInputTransform(enable, src, dst);
            const unsigned long setGle = GetLastError();
            QueryPerformanceCounter(&b);
            // One-shot ground truth (issue #217): the set's return value, its GetLastError, and
            // an immediate read-back, so a publish war or a lying return code is visible in the
            // field log instead of being theorized about. First few publishes only.
            if (ixDbgLogs_ < 4) {
                ++ixDbgLogs_;
                bool gOn = false; RECT gS{}, gD{};
                const bool gOk = host_.getInputTransform(gOn, gS, gD);
                wind::Log(wind::LogLevel::Info, "ixdiag",
                          "set en=%d src=(%ld,%ld,%ld,%ld) ret=%d gle=%lu | get ret=%d en=%d "
                          "src=(%ld,%ld,%ld,%ld)",
                          enable ? 1 : 0, src.left, src.top, src.right, src.bottom,
                          ok ? 1 : 0, setGle, gOk ? 1 : 0, gOn ? 1 : 0,
                          gS.left, gS.top, gS.right, gS.bottom);
            }
            if (!ok) {
                // The return value cries wolf on this rig (issue #217): FALSE while the publish
                // demonstrably lands (read-back tracks our rect). The read-back is the truth.
                bool aOn = false; RECT aS{}, aD{};
                if (host_.getInputTransform(aOn, aS, aD) &&
                    !InputTransformStomped(enable, src.left, src.top, src.right, src.bottom,
                                           aOn, aS.left, aS.top, aS.right, aS.bottom))
                    ok = true;
            }
            noteIxWrite(double(b.QuadPart - a.QuadPart) * 1000.0 / fr.QuadPart, ok);
            if (ok && NudgeAfterPublish(applyLevel, ixPubLevel_)) {
                // A scale-changing publish stops DWM drawing the composed pointer until the next
                // cursor event: give it one, a pixel and back.
                NudgePointer();
            }
            if (ok) ixPubLevel_ = applyLevel;
            if (ok) {
                ixExpectedValid_ = true;
                ixExpectedOn_ = enable;
                ixExpL_ = src.left; ixExpT_ = src.top; ixExpR_ = src.right; ixExpB_ = src.bottom;
            } else {
                // Self-heal (spec constraint 1): a VERIFIED-failed ENABLED publish means this
                // build cannot fix the pointer-framework dead zones - the DESKTOP pick must stop
                // choosing the transform. Games are unaffected (legacy input surfaces).
                // Only ERROR_ACCESS_DENIED (no UIAccess) is permanent. Any other single failed
                // publish is treated as transient: the flag is re-armed from the token probe when
                // the session ends (setActive(false)), so one hiccup no longer disables the
                // desktop pick until restart (review 2026-10-09 #25).
                inputTransformAvailable_ = false;
                if (setGle == ERROR_ACCESS_DENIED) inputXformDenied_ = true;
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
        ixExpectedValid_ = true;
        ixExpectedOn_ = false;
    }
    if (changed) lastInputXformOn_ = cfg.magInputTransform != 0;
    }   // !ex.pauseWrites
    // Edge clip: engaged while genuinely zoomed and the pointer is OURS to manage (not Inspect,
    // whose 1px freeze clip must never be disturbed - ex.clickOverride marks it).
    // Hot like the other knobs: switched off mid-zoom, give the clip back now rather than at the
    // end of the session (issue #274). Gated so an unused edgeClip costs no per-tick syscall.
    if (cfg.edgeClip != 0 || edgeClipActive_)
        edgeClipManage(cfg.edgeClip != 0 && applyLevel > 1.001 && !ex.clickOverride);

    // WELD: the locked (mouselook, pointer hidden) and Inspect regimes park the real pointer on the
    // lens point once per tick (ex.suppressCursorSync is false there). A free session never welds:
    // the pointer is the input and DWM centres on it. Deduped so an idle tick injects nothing.
    // Inspect pins the point via clickOverride; otherwise clickDesktop is monitor-local, so add the
    // monitor origin. Drag-follow (#169) belongs to the render engine. weldedLastFrame_ records
    // whether SetCursorPos REALLY ran, so RunTick can baseline on the weld point only when it did
    // (#169 measured-baseline law; assuming it landed is the unstable-servo bug).
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

    if (ex.cursorLocked && ex.drawCursor) {
        // Inspect mode: the real cursor is frozen at the (overridden) click point, but the thing the
        // user aims with is the LOOK POINT (mapper center). Draw the crosshair (the same design the
        // render model draws) on the look point, NOT on cx/cy - those are pinned to the frozen
        // cursor while Inspect is on. The transform is anchored at the look point (T(L) == L) and
        // this layered window composites unmagnified, so the crosshair sits exactly on the aimed
        // content at any zoom, including the 1x roam.
        updateSpriteLayer();
        sprite_->showCrosshair();
        sprite_->moveTo(r.clickDesktopX + mon_.x, r.clickDesktopY + mon_.y);
        sprite_->keepOnTop();
    } else {
        sprite_->hide();
        if (level > 1.001) {
            // NATIVE CURSOR (issue #369): DWM draws the real pointer into the magnified frame, above
            // every band. Only the hide-cursor hotkey / cursorVisibility=never hides it, the same
            // way Inspect does (blanker for standard shapes, MagShowSystemCursor for app-custom
            // ones).
            if (!ex.drawCursor) {
                if (!cursorHidden_) {
                    blanker_->blank();
                    ShowSystemCursor(FALSE);
                    cursorHidden_ = true;
                }
            } else if (cursorHidden_) {
                ShowSystemCursor(TRUE);
                // A restored-but-still pointer stays invisible until a cursor EVENT, so the nudge
                // follows the restore on the blanker's worker, as at the zoom-out.
                blanker_->restore([] {
                    NudgePointer();
                });
                cursorHidden_ = false;
            }
        }
    }

    if (traceOn_) {
        LARGE_INTEGER qf, qc; QueryPerformanceFrequency(&qf); QueryPerformanceCounter(&qc);
        TxTick& e = traceBuf_[traceHead_ % kTraceCap];
        e.ms = double(qc.QuadPart) * 1000.0 / double(qf.QuadPart);
        e.level = trLevel; e.txX = trTxX; e.offX = trOffX;
        e.wrote = (unsigned char)(txWroteThisTick ? 1 : 0);
        e.changed = (unsigned char)(trChanged ? 1 : 0);
        e.ramping = (unsigned char)(trRamping ? 1 : 0);
        e.warm = (unsigned char)(trWarm ? 1 : 0);
        ++traceHead_;
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
        // THE DEMOTION IS A RACE, AND A BLIND 500ms CADENCE LOSES IT (measured 2026-08-26 with
        // tools/plane_race_probe.ps1: 3 of 6 alt-tab sessions left the game on its overlay plane
        // for ~2.5s, against native Magnifier's 6 of 6 composited on the same machine). While the
        // game holds a hardware plane DWM is not compositing it, so nothing we write can drive the
        // composition rate and every pan start lands late - that IS the field stutter, and its
        // per-session randomness is this race.
        //
        // So the OPENING of a session is asserted hard and the steady state stays calm: the first
        // kGhostAggressiveMs are re-asserted every kGhostFastMs, which is what actually has to win
        // against DWM re-promoting the game as it takes the foreground back. assert_() is already
        // read-first (it transacts only when the ghost is hidden, moved, or stripped of TOPMOST),
        // so a tighter cadence costs three cheap reads per tick, not a z-order transaction.
        static constexpr unsigned long long kGhostAggressiveMs = 2000;
        static constexpr unsigned long long kGhostFastMs       = 100;
        if (ghostSessionStartMs_ == 0) ghostSessionStartMs_ = nowG;
        const bool opening = (nowG - ghostSessionStartMs_) < kGhostAggressiveMs;
        const unsigned long long cadence = opening ? kGhostFastMs : 500;
        if (nowG - lastGhostAssertMs_ >= cadence) { lastGhostAssertMs_ = nowG; mpoGhost_.assert_(); }
    } else {
        ghostSessionStartMs_ = 0;   // next session re-opens with the aggressive window
        mpoGhost_.hide();
    }
}

bool TransformModel::retarget(const MonitorTarget& m) {
    if (m.w <= 0 || m.h <= 0) return false;          // a bogus target would clamp to nothing
    if (m.x == mon_.x && m.y == mon_.y && m.w == mon_.w && m.h == mon_.h) return true;
    wind::Log(wind::LogLevel::Info, "transform", "retarget %dx%d at (%d,%d) -> %dx%d at (%d,%d)",
              mon_.w, mon_.h, mon_.x, mon_.y, m.w, m.h, m.x, m.y);
    edgeClipManage(false);   // the 1 px inset clip describes the OLD monitor: give it back first
    mon_ = m;
    // The cached level/translation describe the OLD geometry, and the write path skips a value it
    // believes DWM already holds - so without this the first write after a resolution change is
    // suppressed and the stale transform stays on screen. It also forces the input-transform rects
    // to be republished, which are likewise sized from mon_.
    resetTransformState();
    // The MPO ghost is created at monitor bounds; leave it correct for the new ones.
    mpoGhost_.hide();
    mpoGhost_.create(mon_.x, mon_.y, mon_.w, mon_.h);
    return true;
}

void TransformModel::shutdown() {
    teardownMag();
    if (sprite_) sprite_->destroy();
    if (blanker_ && blanker_->blanked()) blanker_->restoreSync();   // never exit with blank cursors
    pin_.destroy();
    mpoGhost_.destroy();
    ready_ = false;
}

// Dump the per-tick trace (txTrace=1, diagnostic). Called at session end ON the tick thread, so it
// only copies the ring; a below-normal thread writes the file (#361: no disk I/O on the tick).
void TransformModel::traceDump() {
    if (traceHead_ == 0) return;
    const int n = traceHead_ < kTraceCap ? traceHead_ : kTraceCap;
    const int start = traceHead_ < kTraceCap ? 0 : (traceHead_ % kTraceCap);
    auto* rows = new std::vector<TxTick>();
    rows->reserve((size_t)n);
    for (int i = 0; i < n; ++i) rows->push_back(traceBuf_[(start + i) % kTraceCap]);
    traceHead_ = 0;
    HANDLE th = CreateThread(nullptr, 0, [](LPVOID p) -> DWORD {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        std::unique_ptr<std::vector<TxTick>> v(static_cast<std::vector<TxTick>*>(p));
        WriteTraceCsv(*v);
        return 0;
    }, rows, 0, nullptr);
    if (th) CloseHandle(th); else delete rows;
}

void TransformModel::WriteTraceCsv(const std::vector<TxTick>& rows) {
    const std::wstring dir = wind::ResolveLogDir();
    wchar_t path[MAX_PATH];
    // Forward slash on purpose: Win32 file APIs accept it, and it keeps this string free
    // of backslash escapes (a double-escaped one silently became a TAB here once).
    swprintf(path, MAX_PATH, L"%s/txtrace-%llu.csv", dir.c_str(),
             (unsigned long long)GetTickCount64());
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"w") != 0 || !f) return;
    fprintf(f, "ms,dt,level,txX,offX,wrote,changed,ramping,warm\n");
    double prev = 0.0;
    for (const TxTick& e : rows) {
        const double dt = prev > 0.0 ? (e.ms - prev) : 0.0;
        prev = e.ms;
        fprintf(f, "%.3f,%.3f,%.6f,%d,%d,%d,%d,%d,%d\n",
                e.ms, dt, e.level, e.txX, e.offX,
                (int)e.wrote, (int)e.changed, (int)e.ramping, (int)e.warm);
    }
    fclose(f);
    wind::Log(wind::LogLevel::Info, "txtrace", "wrote %d ticks", (int)rows.size());
}

}  // namespace wind
