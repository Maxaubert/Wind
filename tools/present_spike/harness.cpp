// Present spike harness (issue #69). Creates a fullscreen overlay in a chosen present config and
// runs automated tests against clickprobe.exe (run it first). Results -> %TEMP%\present_spike_results.log
// Usage: harness.exe <blt|dcomp-nolayer|dcomp-layered|none> <skeleton|clickthrough|pacing|latency|latency-baseline>
#include "spike_common.h"
#include "overlay.h"
#include <cstring>
#include <cstdio>

static const char* kResults = "present_spike_results.log";

static const char* ModeName(spike::PresentMode m) {
    switch (m) {
        case spike::PresentMode::Blt:          return "blt";
        case spike::PresentMode::DcompNoLayer: return "dcomp-nolayer";
        case spike::PresentMode::DcompLayered: return "dcomp-layered";
    }
    return "?";
}
static bool ParseMode(const char* a, spike::PresentMode& out) {
    if (strcmp(a, "blt") == 0)           { out = spike::PresentMode::Blt;          return true; }
    if (strcmp(a, "dcomp-nolayer") == 0) { out = spike::PresentMode::DcompNoLayer; return true; }
    if (strcmp(a, "dcomp-layered") == 0) { out = spike::PresentMode::DcompLayered; return true; }
    if (strcmp(a, "none") == 0)          { out = spike::PresentMode::Blt;          return true; } // baseline
    return false;
}

// Pump pending messages and render the overlay for `ms` milliseconds.
static void RenderFor(spike::Overlay& ov, int ms, bool dwmFlush) {
    long long end = spike::QpcNow() + (long long)ms * spike::QpcFreq() / 1000;
    double phase = 0; MSG m;
    while (spike::QpcNow() < end) {
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
        ov.renderFrame(phase, dwmFlush); phase += 0.05;
    }
}

int main(int argc, char** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (argc < 3) {
        printf("usage: harness <blt|dcomp-nolayer|dcomp-layered|none> "
               "<skeleton|clickthrough|pacing|latency|latency-baseline>\n");
        return 2;
    }
    spike::PresentMode mode;
    if (!ParseMode(argv[1], mode)) { printf("bad mode %s\n", argv[1]); return 2; }
    const bool dwmFlush = (mode == spike::PresentMode::Blt);
    const char* test = argv[2];

    if (strcmp(test, "skeleton") == 0) {
        spike::Overlay ov;
        if (!ov.init(mode)) { spike::LogLine(kResults, "skeleton mode=%s ERROR init failed", ModeName(mode)); return 1; }
        RenderFor(ov, 2000, dwmFlush);
        spike::LogLine(kResults, "skeleton mode=%s OK rendered 2s", ModeName(mode));
        ov.shutdown();
        printf("skeleton done; see %%TEMP%%\\present_spike_results.log\n");
        return 0;
    }
    printf("test '%s' not implemented yet\n", test);
    return 0;
}
