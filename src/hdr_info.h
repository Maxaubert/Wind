#pragma once
namespace wind {
// Whether Windows HDR ("Use HDR") is actually ON right now (DisplayConfig
// ADVANCED_COLOR_INFO_2 activeColorMode). False if the API is unavailable (older Windows).
// Pass the target monitor's GDI device name (e.g. L"\\\\.\\DISPLAY2") so a multiMonitor session
// answers for ITS display; null/empty falls back to the first path that answers (mixed
// HDR/SDR monitor setups would otherwise take whichever path enumerated first).
bool GetHdrEnabled(const wchar_t* gdiDeviceName = nullptr);
// SDR white level (nits) of the monitor we are magnifying, so HDR->SDR tonemapping matches what
// DWM composites. This is Windows' "SDR content brightness" slider and the user can move it at any
// time, so it must be RE-READ, never cached for the process lifetime (a stale value is a visible
// brightness step on every zoom - see hdr_scale.h). `gdiDeviceName` is a GDI name like
// "\\\\.\\DISPLAY1"; null/empty or no match falls back to the first active path. Returns 0.0 if the
// query fails, so the caller can keep its last known good value (wind::AcceptSdrWhiteNits).
double GetSDRWhiteNits(const wchar_t* gdiDeviceName);
}
