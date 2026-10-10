#pragma once
#include "magnifier_model.h"
#include "mag_host.h"
#include "comp_pin.h"
#include "cursor_blanker.h"
#include "cursor_sprite.h"
#include <memory>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
namespace wind {
class TransformModel : public IMagnifierModel {
public:
    // Every session draws DWM's own pointer (issue #369, src/native_cursor.h); the sprite window
    // exists only for the Inspect crosshair, and zorderBand / cursorBandAuto place that window.
    TransformModel(bool fastPan, bool smoothPan, int zorderBand, bool cursorBandAuto = false)
        : fastPan_(fastPan), smoothPan_(smoothPan),
          zorderBand_(zorderBand), cursorBandAuto_(cursorBandAuto) {}
    bool initialize(const MonitorTarget& monitor) override;
    // MONITOR GEOMETRY CAN CHANGE UNDER A LIVE SESSION (issue #230). mon_ feeds the clamp bounds in
    // ComputeMagTransform, the crosshair's placement offsets and the MagSetInputTransform rects, and
    // it used to be written once at startup: retarget() was declared render-only, so a display-mode
    // change - a game switching to a lower resolution - left the transform clamping against the old
    // size and the view could be panned off the real desktop.
    bool retarget(const MonitorTarget& m) override;
    void shutdown() override;
    bool ready() const override { return ready_; }
    void hideSystemCursor(bool hide) override;
    void setActive(bool active) override;
    void idleTick() override;                     // keeps the context and the cursor lens warm at 1x
    void present(const MapResult& r, double level, const Config& cfg,
                 const MonitorTarget& mon, const PresentExtras& ex) override;
    bool coversShell() const override { return false; }
    // Written-transform readbacks for the telemetry channel (issue #227): what the tick path
    // last actually applied.
    double writtenLevel() const { return lastLevel_; }
    int    writtenTxX() const { return lastTxX_; }
    int    writtenTxY() const { return lastTxY_; }
    // True when THIS present actually called SetCursorPos (weld executed; not deduped, not
    // suppressed). Only the locked and Inspect regimes weld now; RunTick's #169 measured-baseline
    // logic reads it exactly like RenderEngine::parkedLastFrame().
    bool weldedLastFrame() const { return weldedLastFrame_; }
    // Whether WE currently hide the real pointer (Inspect, or the hide-cursor hotkey in a native
    // session, which present() applies on its own). The tick loop mirrors it into
    // cursorHiddenByUs, the game-inspect tell, instead of tracking the calls itself.
    bool pointerHidden() const { return cursorHidden_; }
    // Whether MagSetInputTransform is usable (probed once at initialize; needs UIAccess). The
    // hybrid DESKTOP pick requires this: without the source-rect input transform, transform
    // desktop sessions have the pointer-framework hover dead zones (POINTER-HITTEST-FINDINGS.md).
    bool inputTransformAvailable() const { return inputTransformAvailable_; }
    // Zoom timeline (#310, zoomTrace): where the last setActive(true) spent its time.
    struct EnterSplit { double ensureMagMs = 0; bool wasWarm = false; };
    EnterSplit lastEnter() const { return lastEnter_; }
    // Whether the DWM magnification context is up right now. Read BEFORE the enter tick's first
    // present (which builds it) to know if a zoom-in starts warm or cold.
    bool contextLive() const { return magUp_; }
    // MPO buster (issue #191). Wanted = show the fullscreen alpha-1 ghost this session (MPO-
    // exposed game session + the mpoBuster knob); exposed = the session could overflow the
    // 16-bit plane field, so the write-site clamp applies whenever the ghost is not verifiably
    // settled. ghostSettled = the fail-closed evidence the pan-wall lift requires.
    void setMpoBusterWanted(bool wanted) { mpoBusterWanted_ = wanted; }
    void setMpoExposed(bool exposed) { mpoExposed_ = exposed; }
    bool mpoGhostSettled() const { return mpoGhost_.settled(GetTickCount64()); }
private:
    bool fastPan_, smoothPan_;
    // Per-tick trace (cfg.txTrace). Fixed ring, no allocation on the tick path.
    struct TxTick { double ms; double level; int txX, offX;
                    unsigned char wrote, changed, ramping, warm; };
    static const int kTraceCap = 8192;
    TxTick traceBuf_[kTraceCap]{};
    int  traceHead_ = 0;
    bool traceOn_ = false;
    void traceDump();
    static void WriteTraceCsv(const std::vector<TxTick>& rows);   // background thread
    int  zorderBand_;                                // crosshair z-band (above the shell); needs UIAccess
    bool cursorBandAuto_ = false;                    // issue #269: band 16 unless the snip overlay is up
    HWND layerFg_ = nullptr;                         // foreground the layer verdict was read for
    int  layerFgBand_ = 0;                           // ...and its z-band
    void updateSpriteLayer();                        // pick the crosshair window for this tick (#269)
    bool ready_ = false;
    bool active_ = false;
    MonitorTarget mon_{};
    MagHost host_;
    CompositionPin pin_;
    MpoGhost mpoGhost_;                              // MPO buster (issue #191)
    bool mpoBusterWanted_ = false;                   // show the ghost this session
    // Edge clip (cfg.edgeClip): session-scoped ClipCursor 1px inside the monitor. See config.h.
    bool edgeClipActive_ = false;
    RECT edgeClipSaved_{};                            // the clip that existed before ours
    RECT edgeClipApplied_{};                          // what we set (dedupe + foreign-change test)
    void edgeClipManage(bool wantActive);
    unsigned long long ghostSessionStartMs_ = 0;     // 0 = not started; drives the opening burst
    bool mpoExposed_ = false;                        // apply the 16-bit write clamp
    unsigned long long lastGhostAssertMs_ = 0;       // 500ms assert cadence
    int  appliedSampling_ = -2;                      // sampling mode DWM currently holds (-2 = unknown)
    int  sampleTryMode_ = -2;                        // sampling mode being attempted (#274)
    unsigned long dwmGenSeen_ = 0;                   // DwmGeneration() last acted on (#396)
    int  sampleTries_ = 0;                           // attempts so far for it (bounded at 3)
    unsigned long long sampleLastTryMs_ = 0;         // when the last attempt ran
    std::unique_ptr<CursorBlanker> blanker_;         // Inspect and the hide-cursor hotkey blank the real pointer
    std::unique_ptr<CursorSprite> sprite_;           // the Inspect crosshair
    unsigned long long lastPinAssertMs_ = 0;
    int  keepAliveTick_ = 0;                         // alternates the tx keep-alive (issue #148)
    bool inputXformWarned_ = false;                  // one-shot warn when MagSetInputTransform fails
    bool lastInputXformOn_ = false;                  // knob edge: disable the OS transform on 1->0
    int  lastOffX_ = 0, lastOffY_ = 0, lastTxX_ = 0, lastTxY_ = 0;   // last applied transform
    double lastLevel_ = 0.0;
    double lastRequestedLevel_ = 0.0;
    double sessionMaxLevel_ = 0.0;      // logged at teardown: scripted-run engagement proof
    unsigned long long lastChangeMs_ = 0;            // when the transform last REALLY changed
    unsigned long long lastWarmMs_ = 0;              // when the last warm pulse CLOSED (issue #246)
    // Magnification context lifetime (issues #148, #369). The context is built at idle and kept for
    // the process (idleTick): a context alone costs nothing, it is the COMPOSED pointer (cursor lens
    // style ON) that taxes every cursor change an app makes (a game toggling its pointer on
    // middle-click: measured 17 spike frames per 14 clicks), and the lens is ON only while zoomed.
    // The context is released at shutdown and when the model is swapped out.
    bool magUp_ = false;
    // Native cursor (issue #369). nativePrimed_: the context's one public write that makes DWM
    // draw the real pointer magnified (the fallback when the cursor lens could not be built).
    // dwmCentreOn_: DWM owns the pan (DWMUpdated TRUE).
    bool nativePrimed_ = false;
    bool dwmCentreOn_ = false;
    bool dwmCentreBroken_ = false;                   // the export is missing or refused: never retry, log once
    bool forceWrite_ = false;                        // a centring switch owes DWM one real write
    bool lensLogged_ = false;                        // one-shot log of the cursor-lens warm-up
    bool lensFailed_ = false;                        // the idle lens build failed: do not retry per tick
    double ixPubLevel_ = 0.0;                        // level of the last input-transform publish (#369)
    double ladderReq_ = 0.0, ladderOut_ = 0.0;       // smooth-zoom ladder: settled request -> held level
    void setDwmCentre(bool on);
    bool cursorHidden_ = false;                      // we called MagShowSystemCursor(FALSE)
    bool haveLastClick_ = false;                     // dedup the per-tick cursor weld
    int  lastClickX_ = 0, lastClickY_ = 0;
    bool weldedLastFrame_ = false;                   // SetCursorPos ran in the last present()
    bool inputTransformAvailable_ = false;           // MagSetInputTransform probe (UIAccess)
    bool uiAccessProbe_ = false;                     // the token's UIAccess bit, the re-arm value
    bool inputXformDenied_ = false;                  // publish failed ERROR_ACCESS_DENIED: stay off
    EnterSplit lastEnter_;
    double restLevel_ = 1.0;                         // cfg.txRestLevel (hot): >1 keeps DWM magnifying
    bool ensureMag();
    void teardownMag();
    void resetTransformState();                      // forget cached values across a teardown
    // Transform WRITE path: always on the tick thread (the API is thread-affine; an async writer
    // was tried and every call failed). Logs per-second max/avg write time.
    void writeTransform(float lvl, int offX, int offY, int tx, int ty, bool fast);
    void noteWrite(double ms, bool ok);
    void noteIxWrite(double ms, bool ok);            // input-transform publish stats (issue #189)
    void noteIxStomp();                              // foreign writer overwrote our publish (#217)
    std::mutex statMx_;
    unsigned long long statLogMs_ = 0;
    double statMaxMs_ = 0.0, statSumMs_ = 0.0;
    int    statCount_ = 0, statFails_ = 0, statOver5_ = 0;
    unsigned long long ixLogMs_ = 0;                 // input-transform publish stats (issue #189)
    double ixMaxMs_ = 0.0, ixSumMs_ = 0.0;
    int    ixCount_ = 0, ixFails_ = 0;
    int    ixTick_ = 0;                              // decimation counter (cfg.ixDecimate)
    bool   ixPending_ = false;                       // motion changed since the last publish
    // Stomp guard (issue #217): what we last VERIFIABLY published into the system input-
    // transform slot. Kept across sessions on purpose - a fresh session's first tick compares
    // the slot against the previous session's disable, so a rect stranded by a dead native
    // Magnifier is caught and overwritten immediately.
    bool   ixExpectedValid_ = false;
    bool   ixExpectedOn_ = false;
    int    ixExpL_ = 0, ixExpT_ = 0, ixExpR_ = 0, ixExpB_ = 0;
    int    ixStomps_ = 0;                            // foreign overwrites seen this log window
    bool   ixStompWarned_ = false;                   // one-shot foreign-writer warn
    int    ixDbgLogs_ = 0;                           // one-shot publish ground-truth logs (#217)
};
}
