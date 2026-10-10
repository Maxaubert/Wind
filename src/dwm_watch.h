#pragma once
// DWM restart watch (issue #396).
//
// Some magnification state lives in DWM alone: the bitmap smoothing flag. win32k keeps its copy
// (a read-back still says "smooth"), but a restarted dwm.exe starts nearest and nobody tells it
// otherwise, so after a DWM crash (e.g. the smooth-sampling acrylic crash, #397) Wind kept zooming
// pixelated until it was restarted. Nothing in the write path notices a restart either: DwmFlush
// just blocks for the few seconds DWM is down. So a background thread watches this session's
// dwm.exe process id (a snapshot enumeration once a second; no handle to dwm.exe is ever opened)
// and bumps a generation counter when it is replaced. Consumers re-apply their DWM-held state when
// the generation changes.
namespace wind {

// Pure: a restart is a NEW process id after a known one. 0 means "not running right now" (DWM is
// down mid-restart), which is neither a restart nor a reason to forget the last id.
inline bool DwmRestarted(unsigned long prevPid, unsigned long pid) {
    return prevPid != 0 && pid != 0 && pid != prevPid;
}

void StartDwmWatch();             // idempotent
void StopDwmWatch();              // ends the thread (Wind exit); a no-op when not started
unsigned long DwmGeneration();    // +1 per observed restart of this session's dwm.exe
}
