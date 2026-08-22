#pragma once
// Inline transform writes from the mouse hook (issue #206, stage 2).
//
// Measured: cursor-move -> transform-write latency is 4.36ms median for Wind against 0.58ms for
// native Magnifier, and our spread (0.42-7.13ms, mean 3.94) is uniform across exactly one 6.94ms
// tick - we are purely waiting for the next tick to notice the cursor moved. Native writes inside
// its WH_MOUSE_LL callback. Stage 1 moved the Magnification runtime onto our hook thread; this is
// the part that uses it.
//
// SINGLE WRITER. While this is armed, the hook owns transform writes completely and the transform
// model does not write position at all. Two writers sampling the cursor at different instants would
// alternate between two positions at tick rate - exactly the wobble #205 removed. The tick thread
// still triggers writes (level ramps, and when the mouse is not moving), but it does so by calling
// the SAME function through the owner thread, so there is one formula and one code path.
//
// The hook path is only safe because #205 made the view a pure function of the cursor: no mapper
// state, no accumulated deltas, no smoothing. There is nothing for the hook to race against.
namespace wind {

class MagHost;

// Published by the tick thread each frame; read by the hook on every mouse move.
struct HookTransformState {
    bool   armed = false;      // transform session live, free cursor on, not inspect, not locked
    double level = 1.0;
    int    monX = 0, monY = 0, monW = 0, monH = 0;
    double maxSrcX = -1.0, maxSrcY = -1.0;   // MPO pan wall (#148/#191), <0 = unbounded
    bool   fastPan = true;     // private channel; the PUBLIC one is 3-9ms and must never run here
    MagHost* host = nullptr;
};

// Tick thread: publish the current state. Cheap; takes a brief writer lock.
void PublishHookTransform(const HookTransformState& s);
// Tick thread: disarm (session ended, mode off, Inspect engaged, game lock detected).
void DisarmHookTransform();
bool HookTransformArmed();

// Runs the write. MUST be on the runtime's owning thread. Returns true if a write went out.
// cursorVirtX/Y: the pointer position in VIRTUAL desktop coords. The hook passes the position
// carried in the event itself, which is fresher than any GetCursorPos we could make.
bool WriteHookTransform(double cursorVirtX, double cursorVirtY);

// Called from MouseProc with the position the EVENT carries - fresher than a GetCursorPos and
// without the round trip. Already on the owning thread, so this writes inline.
bool WriteHookTransformFromEvent(long ptx, long pty);

// Convenience for the tick thread: marshal a write using the current cursor position.
bool RequestHookTransformWrite();

// Diagnostics: how many writes the hook path has issued, and how many the tick path issued.
void HookTransformStats(unsigned long long& hookWrites, unsigned long long& tickWrites);

// FRAME GATE (issue #229). The retired per-event write path was fast (measured 0.77ms response
// vs 5.30ms tick-paced) but wrote 4-5 times per composited frame, so DWM latched whichever
// write landed first while drawing the pointer from its own later sample - content and cursor
// from different instants, i.e. the swim. The documented condition for reviving it is at most
// ONE write per composited frame. The tick loop is DwmFlush-paced while zoomed, so it marks
// each composite here; the hook writes the FIRST move of a frame (full event latency) and
// coalesces the rest (the tick's own write still lands them, so no destination is lost).
void MarkComposite();               // called by the tick loop right after DwmFlush returns
void SetHookFrameGate(bool on);     // txHookWrite == 2

// CONTENT-VS-CURSOR LAG (issue #229). The transform is anchored so T(cursor) == cursor, so if
// it was written for cursor c_w while the pointer has since reached c_now, the content is
// displaced from the pointer by |c_now - c_w| * (level - 1) screen px. That is the whole
// wobble question stated numerically: a lag that stays CONSTANT is invisible (the view simply
// trails by a fixed amount), while a lag that jumps frame to frame is what the eye reads as
// the cursor swimming against the content. Both write paths record the cursor they used here;
// the tick loop samples it at the composite boundary, which is the instant DWM pairs the
// transform with the pointer it draws. Screen capture cannot see any of this - it returns the
// unmagnified desktop surface (measured 2026-08-22), so this is the only way to measure it.
void NoteWriteCursor(double virtX, double virtY);
void GetWriteCursor(double& virtX, double& virtY);

// What the HOOK path last pushed to DWM. The transform model's own cache only records tick
// writes, so any measurement built on it cannot see hook writes at all - which is exactly why
// the sprite-vs-centre check kept reading clean on a build the eye called wobbly. Returns
// false when the hook has written nothing this session (level 0).
bool GetHookLiveTransform(double& level, int& txX, int& txY);

}  // namespace wind
