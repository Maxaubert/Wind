// Present spike harness (issue #69). Creates a fullscreen overlay in a chosen present config and
// runs automated tests against clickprobe.exe (run it first). Results -> %TEMP%\present_spike_results.log
// Usage: harness.exe <blt|dcomp-nolayer|dcomp-layered|none> <skeleton|clickthrough|pacing|latency|latency-baseline>
#include "spike_common.h"
#include "overlay.h"
#include <cstring>
#include <cstdio>
#include <sstream>
#include <string>

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

// Read clickprobe's published client rect (screen coords); return its center.
static bool ProbeCenter(int& cx, int& cy) {
    std::string s = spike::ReadAll("present_spike_probe_rect.txt");
    long l, t, r, b;
    if (s.empty() || sscanf_s(s.c_str(), "%ld %ld %ld %ld", &l, &t, &r, &b) != 4) return false;
    cx = (int)((l + r) / 2); cy = (int)((t + b) / 2);
    return true;
}

// Synthesize an absolute left click at a screen point via SendInput (virtual-desktop normalized).
static void ClickAt(int sx, int sy) {
    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    auto norm = [](int v, int origin, int span) { return (LONG)(((double)(v - origin) * 65535.0) / (span > 1 ? span - 1 : 1)); };
    INPUT in[3] = {};
    in[0].type = INPUT_MOUSE;
    in[0].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    in[0].mi.dx = norm(sx, vx, vw); in[0].mi.dy = norm(sy, vy, vh);
    in[1].type = INPUT_MOUSE; in[1].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    in[2].type = INPUT_MOUSE; in[2].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput(3, in, sizeof(INPUT));
}

// Count "<qpc> DOWN" lines in the probe log with qpc >= since.
static int CountDownsSince(long long since) {
    std::istringstream in(spike::ReadAll("present_spike_probe.log"));
    std::string line; int count = 0;
    while (std::getline(in, line)) {
        long long q; char tag[16] = {};
        if (sscanf_s(line.c_str(), "%lld %15s", &q, tag, (unsigned)sizeof(tag)) == 2)
            if (q >= since && strcmp(tag, "DOWN") == 0) count++;
    }
    return count;
}

// Return the first "<qpc> DOWN" qpc >= since, or 0 if none yet.
static long long FirstDownSince(long long since) {
    std::istringstream in(spike::ReadAll("present_spike_probe.log"));
    std::string line;
    while (std::getline(in, line)) {
        long long q; char tag[16] = {};
        if (sscanf_s(line.c_str(), "%lld %15s", &q, tag, (unsigned)sizeof(tag)) == 2)
            if (q >= since && strcmp(tag, "DOWN") == 0) return q;
    }
    return 0;
}

static int DetectHz() {
    DEVMODEW dm{}; dm.dmSize = sizeof(dm);
    if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)
        return (int)dm.dmDisplayFrequency;
    return 60;
}

static void TestPacing(spike::PresentMode mode, bool dwmFlush) {
    spike::Overlay ov;
    if (!ov.init(mode)) { spike::LogLine(kResults, "pacing mode=%s ERROR init failed", ModeName(mode)); return; }
    RenderFor(ov, 400, dwmFlush);            // warmup

    const int hz = DetectHz();
    const double target = 1.0 / hz;
    const long long freq = spike::QpcFreq();
    double elapsed = 0, sum = 0, maxd = 0, phase = 0;
    int frames = 0, hitch = 0, big = 0;
    long long a = spike::QpcNow(); MSG m;
    while (elapsed < 4.0) {
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
        ov.renderFrame(phase, dwmFlush); phase += 0.05;
        long long b = spike::QpcNow();
        double dt = (double)(b - a) / freq; a = b;
        elapsed += dt; sum += dt; frames++;
        if (dt > maxd) maxd = dt;
        if (dt > target * 1.5) hitch++;
        if (dt > target * 2.5) big++;
    }
    spike::LogLine(kResults,
        "pacing mode=%s hz=%d frames=%d fps=%.1f avgMs=%.2f maxMs=%.2f hitch1.5x=%d big2.5x=%d",
        ModeName(mode), hz, frames, frames / elapsed, sum / frames * 1000.0, maxd * 1000.0, hitch, big);
    ov.shutdown();
}

static void TestClickthrough(spike::PresentMode mode, bool dwmFlush) {
    int cx, cy;
    if (!ProbeCenter(cx, cy)) {
        spike::LogLine(kResults, "clickthrough mode=%s ERROR no probe rect (run clickprobe.exe first)", ModeName(mode));
        return;
    }
    spike::Overlay ov;
    if (!ov.init(mode)) { spike::LogLine(kResults, "clickthrough mode=%s ERROR init failed", ModeName(mode)); return; }
    RenderFor(ov, 500, dwmFlush);            // let the overlay compose on top of the probe
    long long t0 = spike::QpcNow();
    ClickAt(cx, cy);
    RenderFor(ov, 300, dwmFlush);            // keep composing while the click propagates
    Sleep(50);
    int downs = CountDownsSince(t0);
    spike::LogLine(kResults, "clickthrough mode=%s result=%s (probe DOWNs after click=%d)",
                   ModeName(mode), downs > 0 ? "PASS" : "FAIL", downs);
    ov.shutdown();
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
    if (strcmp(test, "clickthrough") == 0) { TestClickthrough(mode, dwmFlush); }
    else if (strcmp(test, "pacing") == 0)  { TestPacing(mode, dwmFlush); }
    else { printf("test '%s' not implemented yet\n", test); return 0; }
    printf("%s done; see %%TEMP%%\\present_spike_results.log\n", test);
    return 0;
}
