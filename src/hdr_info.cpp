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

// SDR white level (nits) for the active HDR path, so HDR->SDR tonemapping matches the desktop
// automatically. nits = SDRWhiteLevel / 1000 * 80. Returns a default if the query fails.
double GetSDRWhiteNits() {
    UINT32 nPath = 0, nMode = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &nPath, &nMode) != ERROR_SUCCESS)
        return 200.0;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(nPath);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(nMode);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &nPath, paths.data(), &nMode, modes.data(),
                           nullptr) != ERROR_SUCCESS)
        return 200.0;
    for (UINT32 i = 0; i < nPath; ++i) {
        DISPLAYCONFIG_SDR_WHITE_LEVEL wl{};
        wl.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
        wl.header.size = sizeof(wl);
        wl.header.adapterId = paths[i].targetInfo.adapterId;
        wl.header.id = paths[i].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&wl.header) == ERROR_SUCCESS && wl.SDRWhiteLevel > 0)
            return wl.SDRWhiteLevel / 1000.0 * 80.0;
    }
    return 200.0;
}

}
