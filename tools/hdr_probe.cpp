// Probe the display's advanced-color signals to find the one that means "Use HDR is ON"
// (vs Automatic Color Management / WCG, which can be on while HDR is off on Win11 24H2+).
// Build: cl /nologo /std:c++17 /EHsc /DUNICODE /D_UNICODE tools\hdr_probe.cpp ^
//        /Fe:hdr_probe.exe /link user32.lib
#include <windows.h>
#include <cstdio>
#include <vector>

// ADVANCED_COLOR_INFO_2 (Win11 24H2+) isn't in older SDK headers; define it. activeColorMode
// distinguishes SDR(0) / WCG(1) / HDR(2) - the signal we actually want.
#ifndef WIND_ACINFO2
#define WIND_ACINFO2
typedef struct _WIND_DC_ACINFO2 {
    DISPLAYCONFIG_DEVICE_INFO_HEADER header;
    union {
        struct {
            UINT32 advancedColorSupported : 1;
            UINT32 advancedColorActive : 1;
            UINT32 reserved1 : 1;
            UINT32 automaticColorManagementSupported : 1;
            UINT32 highDynamicRangeSupported : 1;
            UINT32 highDynamicRangeUserEnabled : 1;
            UINT32 wideColorSupported : 1;
            UINT32 wideColorUserEnabled : 1;
            UINT32 reserved : 24;
        };
        UINT32 value;
    };
    INT32 activeColorMode;   // 0 = SDR, 1 = WCG, 2 = HDR
} WIND_DC_ACINFO2;
static const int WIND_GET_ACINFO2 = 15;  // DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO_2
#endif

int main() {
    UINT32 nP = 0, nM = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &nP, &nM) != ERROR_SUCCESS) { printf("buf sizes fail\n"); return 1; }
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(nP);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(nM);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &nP, paths.data(), &nM, modes.data(), nullptr) != ERROR_SUCCESS) { printf("query fail\n"); return 1; }
    for (UINT32 i = 0; i < nP; ++i) {
        auto& t = paths[i].targetInfo;
        DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO ci{};
        ci.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
        ci.header.size = sizeof(ci); ci.header.adapterId = t.adapterId; ci.header.id = t.id;
        LONG r1 = DisplayConfigGetDeviceInfo(&ci.header);
        printf("path %u: ADVANCED_COLOR_INFO r=%ld value=0x%08X supported=%u enabled=%u wideEnforced=%u forceDisabled=%u\n",
               i, r1, ci.value, ci.advancedColorSupported, ci.advancedColorEnabled, ci.wideColorEnforced, ci.advancedColorForceDisabled);
        DISPLAYCONFIG_SDR_WHITE_LEVEL wl{};
        wl.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
        wl.header.size = sizeof(wl); wl.header.adapterId = t.adapterId; wl.header.id = t.id;
        LONG r2 = DisplayConfigGetDeviceInfo(&wl.header);
        printf("        SDR_WHITE_LEVEL r=%ld level=%u nits=%.1f\n", r2, wl.SDRWhiteLevel, wl.SDRWhiteLevel / 1000.0 * 80.0);
        DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO_2 c2{};
        c2.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO_2;
        c2.header.size = sizeof(c2); c2.header.adapterId = t.adapterId; c2.header.id = t.id;
        LONG r3 = DisplayConfigGetDeviceInfo(&c2.header);
        printf("        ACINFO2(real SDK, type=%d size=%zu) r=%ld activeColorMode=%d (0=SDR 1=WCG 2=HDR) hdrUserEnabled=%u\n",
               (int)DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO_2, sizeof(c2), r3,
               (int)c2.activeColorMode, c2.highDynamicRangeUserEnabled);
    }
    return 0;
}
