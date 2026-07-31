#include "hdr_info.h"
#include <windows.h>
#include <vector>
namespace wind {

// Whether Windows HDR ("Use HDR") is actually ON right now. Uses ADVANCED_COLOR_INFO_2's
// activeColorMode (Win11 24H2+), which distinguishes SDR/WCG/HDR. The older
// advancedColorEnabled flag is unreliable here - it reads true when Automatic Color Management
// is on even though "Use HDR" is off (which made us wrongly tonemap and dim SDR). DisplayConfig
// is queried live (not DXGI-cached), so re-checking on duplication-recreate also catches
// runtime HDR toggles. Returns false if the API is unavailable (older Windows) -> SDR path.
bool GetHdrEnabled() {
    UINT32 nPath = 0, nMode = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &nPath, &nMode) != ERROR_SUCCESS)
        return false;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(nPath);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(nMode);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &nPath, paths.data(), &nMode, modes.data(),
                           nullptr) != ERROR_SUCCESS)
        return false;
    for (UINT32 i = 0; i < nPath; ++i) {
        DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO_2 ci{};
        ci.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO_2;
        ci.header.size = sizeof(ci);
        ci.header.adapterId = paths[i].targetInfo.adapterId;
        ci.header.id = paths[i].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&ci.header) == ERROR_SUCCESS)
            return ci.activeColorMode == DISPLAYCONFIG_ADVANCED_COLOR_MODE_HDR;
    }
    return false;
}

// Does this path drive the given GDI device (e.g. "\\.\DISPLAY1")? The source name is the link
// between DisplayConfig and the GDI/DXGI device names the rest of Wind works in.
static bool PathIsDevice(const DISPLAYCONFIG_PATH_INFO& p, const wchar_t* gdiDeviceName) {
    DISPLAYCONFIG_SOURCE_DEVICE_NAME sn{};
    sn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
    sn.header.size = sizeof(sn);
    sn.header.adapterId = p.sourceInfo.adapterId;
    sn.header.id = p.sourceInfo.id;
    return DisplayConfigGetDeviceInfo(&sn.header) == ERROR_SUCCESS &&
           lstrcmpiW(sn.viewGdiDeviceName, gdiDeviceName) == 0;
}

// Read one path's SDR white level in nits. nits = SDRWhiteLevel / 1000 * 80. 0.0 = unavailable.
static double PathSdrWhiteNits(const DISPLAYCONFIG_PATH_INFO& p) {
    DISPLAYCONFIG_SDR_WHITE_LEVEL wl{};
    wl.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
    wl.header.size = sizeof(wl);
    wl.header.adapterId = p.targetInfo.adapterId;
    wl.header.id = p.targetInfo.id;
    if (DisplayConfigGetDeviceInfo(&wl.header) != ERROR_SUCCESS || wl.SDRWhiteLevel == 0) return 0.0;
    return wl.SDRWhiteLevel / 1000.0 * 80.0;
}

// SDR white level (nits) of the monitor we are magnifying. Matched by GDI device name so a
// multiMonitor session tonemaps against ITS display's slider, not whichever path enumerated first
// (two HDR monitors can sit at different SDR white levels). Falls back to the first path that
// answers. Returns 0.0 on failure - the caller keeps its previous value rather than jumping to a
// default, which would be its own visible brightness step.
double GetSDRWhiteNits(const wchar_t* gdiDeviceName) {
    UINT32 nPath = 0, nMode = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &nPath, &nMode) != ERROR_SUCCESS)
        return 0.0;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(nPath);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(nMode);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &nPath, paths.data(), &nMode, modes.data(),
                           nullptr) != ERROR_SUCCESS)
        return 0.0;
    if (gdiDeviceName && gdiDeviceName[0]) {
        for (UINT32 i = 0; i < nPath; ++i)
            if (PathIsDevice(paths[i], gdiDeviceName)) {
                double nits = PathSdrWhiteNits(paths[i]);
                if (nits > 0.0) return nits;
                break;   // right monitor, no answer -> fall through to the first that has one
            }
    }
    for (UINT32 i = 0; i < nPath; ++i) {
        double nits = PathSdrWhiteNits(paths[i]);
        if (nits > 0.0) return nits;
    }
    return 0.0;
}

}
