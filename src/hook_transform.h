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
    int    minIntervalMs = 8;  // trailing-edge throttle; 0 = write every event (measured bad)
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

}  // namespace wind
