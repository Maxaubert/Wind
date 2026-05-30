# DComp Present Spike Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a standalone harness that decides, by measurement, whether a DirectComposition flip-model overlay can replace the blt+DwmFlush present path without breaking cross-process click-through or regressing background-app responsiveness.

**Architecture:** Two standalone executables under `tools/present_spike/`, built by a new `build.bat spike` target, fully isolated from `Wind.exe`. `clickprobe.exe` is a separate-process click target that logs received clicks with QPC timestamps. `harness.exe` creates a fullscreen overlay in one of three present configs (`blt`, `dcomp-nolayer`, `dcomp-layered`) and runs three automated tests against the probe: click-through (real cross-process delivery, not `WindowFromPoint`), present pacing, and input latency. Results are appended to `%TEMP%\present_spike_results.log`.

**Tech Stack:** C++17, MSVC `cl.exe`, Win32, D3D11, DXGI (`CreateSwapChainForComposition`), DirectComposition (`dcomp.lib`), `SendInput`. No unit-test framework: each component is verified by building and running it and reading the emitted log files.

**Spec:** `docs/superpowers/specs/2026-05-29-dcomp-present-spike-design.md`

---

## File Structure

- Create: `tools/present_spike/spike_common.h` - shared inline helpers (QPC, temp-file log read/write). Header-only so both exes include it without a shared TU.
- Create: `tools/present_spike/clickprobe.cpp` - the separate-process click target (`clickprobe.exe`).
- Create: `tools/present_spike/overlay.h` - the `Overlay` class interface (present config under test).
- Create: `tools/present_spike/overlay.cpp` - `Overlay` implementation (all D3D/DXGI/DComp here).
- Create: `tools/present_spike/harness.cpp` - `main`, arg parse, the three tests (`harness.exe`).
- Create: `tools/present_spike/README.md` - build line + runbook for the real-workload measurement.
- Modify: `build.bat` - add a `spike` target that compiles both exes.

---

## Task 1: Shared helpers + build target (toolchain smoke test)

**Files:**
- Create: `tools/present_spike/spike_common.h`
- Modify: `build.bat` (add `spike` dispatch + block)
- Create: `tools/present_spike/clickprobe.cpp` (temporary minimal stub, replaced in Task 2)

- [ ] **Step 1: Create the shared helper header**

Create `tools/present_spike/spike_common.h`:

```cpp
#pragma once
#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <string>

// Shared, header-only helpers for the present spike (clickprobe + harness). Inline so both
// single-TU executables can include this without a common compilation unit.
namespace spike {

inline long long QpcNow()  { LARGE_INTEGER c; QueryPerformanceCounter(&c); return c.QuadPart; }
inline long long QpcFreq() { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f.QuadPart; }

// Full path in %TEMP% for a spike file name (so it works regardless of the working directory).
inline std::string TempPath(const char* name) {
    char dir[MAX_PATH]; DWORD n = GetTempPathA(MAX_PATH, dir);
    std::string p(dir, (n && n <= MAX_PATH) ? n : 0);
    p += name;
    return p;
}

// Append a printf-formatted line to a %TEMP% file.
inline void LogLine(const char* file, const char* fmt, ...) {
    std::string path = TempPath(file);
    FILE* f = nullptr; if (fopen_s(&f, path.c_str(), "a") != 0 || !f) return;
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f); fclose(f);
}

// Overwrite a %TEMP% file with a single printf-formatted line.
inline void WriteLine(const char* file, const char* fmt, ...) {
    std::string path = TempPath(file);
    FILE* f = nullptr; if (fopen_s(&f, path.c_str(), "w") != 0 || !f) return;
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f); fclose(f);
}

// Read the entire contents of a %TEMP% file (empty string if missing).
inline std::string ReadAll(const char* file) {
    std::string path = TempPath(file);
    FILE* f = nullptr; if (fopen_s(&f, path.c_str(), "r") != 0 || !f) return {};
    std::string out; char buf[512]; size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    fclose(f);
    return out;
}

inline void DeleteTemp(const char* file) { DeleteFileA(TempPath(file).c_str()); }

} // namespace spike
```

- [ ] **Step 2: Create a temporary stub clickprobe so the build target has something to compile**

Create `tools/present_spike/clickprobe.cpp`:

```cpp
#include "spike_common.h"
int main() { spike::LogLine("present_spike_results.log", "clickprobe stub ok"); return 0; }
```

- [ ] **Step 3: Add the `spike` target to `build.bat`**

In `build.bat`, add this line immediately after the existing `if /i "%1"=="config" goto :config` line (around line 27):

```bat
if /i "%1"=="spike" goto :spike
```

Then add this block at the end of the file (after the `:check` block, after line 80):

```bat
rem --- Present spike harness (tools\present_spike): clickprobe.exe + harness.exe -----
rem    Standalone DComp/blt present experiment for issue #69. Not part of Wind.exe.
:spike
cl /nologo /std:c++17 /EHsc /O2 /DUNICODE /D_UNICODE ^
   tools\present_spike\clickprobe.cpp ^
   /Fe:clickprobe.exe ^
   /link user32.lib gdi32.lib
if errorlevel 1 exit /b 1
if exist tools\present_spike\harness.cpp (
  cl /nologo /std:c++17 /EHsc /O2 /DUNICODE /D_UNICODE ^
     tools\present_spike\harness.cpp tools\present_spike\overlay.cpp ^
     /Fe:harness.exe ^
     /link d3d11.lib dxgi.lib dcomp.lib dxguid.lib user32.lib gdi32.lib dwmapi.lib
)
exit /b %errorlevel%
```

(The `if exist` guard lets Task 1 build the probe before `harness.cpp`/`overlay.cpp` exist.)

- [ ] **Step 4: Build to verify the toolchain + target work**

Run: `build.bat spike`
Expected: compiles with no errors, produces `clickprobe.exe` in the repo root, exit code 0.

- [ ] **Step 5: Run the stub to verify logging works**

Run: `clickprobe.exe` then `type "%TEMP%\present_spike_results.log"`
Expected: the file contains a line `clickprobe stub ok`.

- [ ] **Step 6: Commit**

```bash
git add tools/present_spike/spike_common.h tools/present_spike/clickprobe.cpp build.bat
git commit -m "spike: scaffold present_spike build target + shared helpers (#69)"
```

---

## Task 2: clickprobe.exe (the cross-process click target)

**Files:**
- Modify: `tools/present_spike/clickprobe.cpp` (replace the stub)

- [ ] **Step 1: Replace `clickprobe.cpp` with the real probe**

Overwrite `tools/present_spike/clickprobe.cpp`:

```cpp
// Separate-process click target for the present spike. Creates a visible topmost window whose
// whole client area is a "button"; logs a QPC timestamp on every left button down/up to
// %TEMP%\present_spike_probe.log, and writes its client rect (screen coords) to
// %TEMP%\present_spike_probe_rect.txt so the harness knows where to click. Being a DIFFERENT
// process is what makes the harness's click-through test valid. Run: clickprobe.exe [--seconds N]
#include "spike_common.h"
#include <cstring>
#include <cstdlib>

static const char* kProbeLog  = "present_spike_probe.log";
static const char* kProbeRect = "present_spike_probe_rect.txt";

static LRESULT CALLBACK ProbeProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_LBUTTONDOWN: spike::LogLine(kProbeLog, "%lld DOWN", spike::QpcNow()); return 0;
        case WM_LBUTTONUP:   spike::LogLine(kProbeLog, "%lld UP",   spike::QpcNow()); return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
            RECT rc; GetClientRect(h, &rc);
            HBRUSH b = CreateSolidBrush(RGB(0, 160, 0));
            FillRect(dc, &rc, b); DeleteObject(b);
            EndPaint(h, &ps); return 0;
        }
        case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

int main(int argc, char** argv) {
    int seconds = 0;
    for (int i = 1; i < argc; ++i)
        if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) seconds = atoi(argv[++i]);

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    spike::DeleteTemp(kProbeLog);   // start clean each run

    WNDCLASSW wc{}; wc.lpfnWndProc = ProbeProc; wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"PresentSpikeProbe"; wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);

    const int x = 300, y = 300, w = 400, h = 400;
    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST, wc.lpszClassName, L"present-spike probe",
        WS_OVERLAPPEDWINDOW, x, y, w, h, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) return 1;
    ShowWindow(hwnd, SW_SHOWNORMAL);
    SetForegroundWindow(hwnd);

    // Publish the client-area rect in screen coordinates so the harness can click its center.
    RECT cr; GetClientRect(hwnd, &cr);
    POINT tl{ cr.left, cr.top }, br{ cr.right, cr.bottom };
    ClientToScreen(hwnd, &tl); ClientToScreen(hwnd, &br);
    spike::WriteLine(kProbeRect, "%ld %ld %ld %ld", tl.x, tl.y, br.x, br.y);

    long long deadline = seconds > 0 ? spike::QpcNow() + (long long)seconds * spike::QpcFreq() : 0;
    MSG msg;
    for (;;) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) return 0;
            TranslateMessage(&msg); DispatchMessageW(&msg);
        }
        if (deadline && spike::QpcNow() >= deadline) break;
        Sleep(5);
    }
    return 0;
}
```

- [ ] **Step 2: Build**

Run: `build.bat spike`
Expected: `clickprobe.exe` rebuilt, exit code 0.

- [ ] **Step 3: Run the probe for a few seconds and verify it publishes its rect**

Run: `start "" clickprobe.exe --seconds 5` then immediately `timeout /t 1 >nul & type "%TEMP%\present_spike_probe_rect.txt"`
Expected: a green window appears near (300,300); the rect file contains four integers (e.g. `308 350 700 742`), the client rect in screen coords.

- [ ] **Step 4: Verify clicks are logged**

While the probe window is up (re-run `clickprobe.exe --seconds 15` if it closed), click on the green window once, then run: `type "%TEMP%\present_spike_probe.log"`
Expected: at least one `<number> DOWN` line and one `<number> UP` line.

- [ ] **Step 5: Commit**

```bash
git add tools/present_spike/clickprobe.cpp
git commit -m "spike: clickprobe.exe cross-process click target (#69)"
```

---

## Task 3: Overlay class + harness skeleton (the three present configs render)

**Files:**
- Create: `tools/present_spike/overlay.h`
- Create: `tools/present_spike/overlay.cpp`
- Create: `tools/present_spike/harness.cpp` (skeleton: parse mode, init overlay, render 2 s, exit)

- [ ] **Step 1: Create `overlay.h`**

Create `tools/present_spike/overlay.h`:

```cpp
#pragma once
#include <windows.h>

namespace spike {

// The three present configurations under test (issue #69).
//  Blt          - blt-model DXGI_SWAP_EFFECT_DISCARD on a WS_EX_LAYERED window, paced by DwmFlush
//                 (the current shipping path / baseline).
//  DcompNoLayer - flip-model CreateSwapChainForComposition via DComp on a WS_EX_NOREDIRECTIONBITMAP
//                 (NOT layered) window. The #11 config: smooth, but clicks were eaten.
//  DcompLayered - the same flip-model DComp visual but on a WS_EX_LAYERED window. The #24 config:
//                 clicks worked, but reverted on (unmeasured) background lag.
enum class PresentMode { Blt, DcompNoLayer, DcompLayered };

// A fullscreen present overlay in one of the three configs. Renders a recognizable semi-transparent
// frame; present is config-specific. All D3D/DXGI/DComp headers stay inside overlay.cpp.
class Overlay {
public:
    Overlay();
    ~Overlay();
    Overlay(const Overlay&) = delete;
    Overlay& operator=(const Overlay&) = delete;

    bool init(PresentMode mode);
    // Render + present one frame. `phase` shifts the fill color so successive frames differ (so the
    // compositor sees real change during the pacing test). `paceWithDwmFlush` (blt only) presents
    // immediately then blocks on DwmFlush, mirroring the shipping default. Returns false on failure.
    bool renderFrame(double phase, bool paceWithDwmFlush);
    void shutdown();
    PresentMode mode() const;

private:
    struct State;
    State* s_;
};

} // namespace spike
```

- [ ] **Step 2: Create `overlay.cpp`**

Create `tools/present_spike/overlay.cpp`:

```cpp
#include "overlay.h"
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dcomp.h>
#include <dwmapi.h>
#include <wrl/client.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "dwmapi.lib")

using Microsoft::WRL::ComPtr;

namespace spike {

struct Overlay::State {
    PresentMode mode = PresentMode::Blt;
    HWND hwnd = nullptr;
    int sw = 0, sh = 0;
    bool layered = false;
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<IDXGISwapChain>  swapBlt;    // blt path
    ComPtr<IDXGISwapChain1> swapFlip;   // dcomp path
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<IDCompositionDevice> dcomp;
    ComPtr<IDCompositionTarget> target;
    ComPtr<IDCompositionVisual> visual;
};

// HTTRANSPARENT here is belt-and-braces; the real cross-process behavior is decided by the window
// styles, which is exactly what this spike measures.
static LRESULT CALLBACK OverlayProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCHITTEST) return HTTRANSPARENT;
    return DefWindowProcW(h, m, w, l);
}

Overlay::Overlay() : s_(new State()) {}
Overlay::~Overlay() { shutdown(); delete s_; s_ = nullptr; }
PresentMode Overlay::mode() const { return s_->mode; }

bool Overlay::init(PresentMode mode) {
    s_->mode = mode;
    s_->sw = GetSystemMetrics(SM_CXSCREEN);
    s_->sh = GetSystemMetrics(SM_CYSCREEN);

    static const wchar_t* kClass = L"PresentSpikeOverlay";
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = OverlayProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kClass;
        wc.hCursor = nullptr;
        RegisterClassW(&wc);
        registered = true;
    }

    DWORD ex = WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT;
    switch (mode) {
        case PresentMode::Blt:          ex |= WS_EX_LAYERED;             s_->layered = true; break;
        case PresentMode::DcompLayered: ex |= WS_EX_LAYERED;             s_->layered = true; break;
        case PresentMode::DcompNoLayer: ex |= WS_EX_NOREDIRECTIONBITMAP;                     break;
    }
    s_->hwnd = CreateWindowExW(ex, kClass, L"present-spike overlay", WS_POPUP,
        0, 0, s_->sw, s_->sh, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!s_->hwnd) return false;

    SetWindowDisplayAffinity(s_->hwnd, WDA_EXCLUDEFROMCAPTURE);  // invariant: never capture ourselves
    if (s_->layered) SetLayeredWindowAttributes(s_->hwnd, 0, 255, LWA_ALPHA);

    D3D_FEATURE_LEVEL fl{};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
        s_->dev.ReleaseAndGetAddressOf(), &fl, s_->ctx.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return false;

    if (mode == PresentMode::Blt) {
        ComPtr<IDXGIDevice1> dxgiDev; s_->dev.As(&dxgiDev);
        dxgiDev->SetMaximumFrameLatency(1);
        ComPtr<IDXGIAdapter> adapter; dxgiDev->GetAdapter(&adapter);
        ComPtr<IDXGIFactory> factory; adapter->GetParent(IID_PPV_ARGS(&factory));
        DXGI_SWAP_CHAIN_DESC scd{};
        scd.BufferDesc.Width = s_->sw; scd.BufferDesc.Height = s_->sh;
        scd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        scd.SampleDesc.Count = 1;
        scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        scd.BufferCount = 1;
        scd.OutputWindow = s_->hwnd;
        scd.Windowed = TRUE;
        scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        hr = factory->CreateSwapChain(s_->dev.Get(), &scd, s_->swapBlt.ReleaseAndGetAddressOf());
        if (FAILED(hr)) return false;
        factory->MakeWindowAssociation(s_->hwnd, DXGI_MWA_NO_ALT_ENTER);
        ComPtr<ID3D11Texture2D> back;
        if (FAILED(s_->swapBlt->GetBuffer(0, IID_PPV_ARGS(&back)))) return false;
        if (FAILED(s_->dev->CreateRenderTargetView(back.Get(), nullptr, s_->rtv.ReleaseAndGetAddressOf()))) return false;
    } else {
        ComPtr<IDXGIDevice> dxgiDev; s_->dev.As(&dxgiDev);
        ComPtr<IDXGIAdapter> adapter; dxgiDev->GetAdapter(&adapter);
        ComPtr<IDXGIFactory2> factory; adapter->GetParent(IID_PPV_ARGS(&factory));
        DXGI_SWAP_CHAIN_DESC1 scd{};
        scd.Width = s_->sw; scd.Height = s_->sh;
        scd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        scd.SampleDesc.Count = 1;
        scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        scd.BufferCount = 2;
        scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        scd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
        scd.Scaling = DXGI_SCALING_STRETCH;
        hr = factory->CreateSwapChainForComposition(s_->dev.Get(), &scd, nullptr, s_->swapFlip.ReleaseAndGetAddressOf());
        if (FAILED(hr)) return false;
        hr = DCompositionCreateDevice(dxgiDev.Get(), IID_PPV_ARGS(&s_->dcomp));
        if (FAILED(hr)) return false;
        if (FAILED(s_->dcomp->CreateTargetForHwnd(s_->hwnd, TRUE, s_->target.ReleaseAndGetAddressOf()))) return false;
        if (FAILED(s_->dcomp->CreateVisual(s_->visual.ReleaseAndGetAddressOf()))) return false;
        s_->visual->SetContent(s_->swapFlip.Get());
        s_->target->SetRoot(s_->visual.Get());
        if (FAILED(s_->dcomp->Commit())) return false;
    }

    ShowWindow(s_->hwnd, SW_SHOWNOACTIVATE);
    return true;
}

bool Overlay::renderFrame(double phase, bool paceWithDwmFlush) {
    float t = (float)(phase - (double)(long long)phase);   // 0..1 sawtooth so frames differ
    float a = 0.5f;                                         // 50% premultiplied: rgb already * alpha
    float col[4] = { t * a, 0.10f * a, 0.30f * a, a };

    if (s_->mode != PresentMode::Blt) {
        // Flip-model: re-acquire the current back buffer's RTV each frame (buffer rotates on flip).
        ComPtr<ID3D11Texture2D> back;
        if (FAILED(s_->swapFlip->GetBuffer(0, IID_PPV_ARGS(&back)))) return false;
        s_->rtv.Reset();
        if (FAILED(s_->dev->CreateRenderTargetView(back.Get(), nullptr, s_->rtv.ReleaseAndGetAddressOf()))) return false;
    }
    s_->ctx->OMSetRenderTargets(1, s_->rtv.GetAddressOf(), nullptr);
    s_->ctx->ClearRenderTargetView(s_->rtv.Get(), col);

    if (s_->mode == PresentMode::Blt) {
        HRESULT hr = s_->swapBlt->Present(0, 0);
        if (paceWithDwmFlush) DwmFlush();
        return SUCCEEDED(hr);
    }
    return SUCCEEDED(s_->swapFlip->Present(1, 0));   // flip-model paces natively on vsync
}

void Overlay::shutdown() {
    if (!s_) return;
    s_->rtv.Reset(); s_->visual.Reset(); s_->target.Reset(); s_->dcomp.Reset();
    s_->swapFlip.Reset(); s_->swapBlt.Reset(); s_->ctx.Reset(); s_->dev.Reset();
    if (s_->hwnd) { DestroyWindow(s_->hwnd); s_->hwnd = nullptr; }
}

} // namespace spike
```

- [ ] **Step 3: Create the harness skeleton**

Create `tools/present_spike/harness.cpp`:

```cpp
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
```

- [ ] **Step 4: Build both exes**

Run: `build.bat spike`
Expected: `clickprobe.exe` and `harness.exe` produced, exit code 0. (If `dcomp.h` is missing, the Windows SDK is incomplete - it ships with 10.0.26100.0; stop and report.)

- [ ] **Step 5: Run the skeleton for each mode and verify each composes**

Run each of:
```
harness.exe blt skeleton
harness.exe dcomp-nolayer skeleton
harness.exe dcomp-layered skeleton
```
Then: `type "%TEMP%\present_spike_results.log"`
Expected: three `skeleton mode=... OK rendered 2s` lines. Visually, each run flashes a translucent magenta-ish tint over the screen for ~2 s with no crash. Any `ERROR init failed` means that config cannot be created on this machine - record which.

- [ ] **Step 6: Commit**

```bash
git add tools/present_spike/overlay.h tools/present_spike/overlay.cpp tools/present_spike/harness.cpp
git commit -m "spike: overlay class (blt/dcomp x layered) + harness skeleton (#69)"
```

---

## Task 4: Click-through test (the corrected, real cross-process check)

**Files:**
- Modify: `tools/present_spike/harness.cpp` (add helpers + `clickthrough` test, wire into `main`)

- [ ] **Step 1: Add the click + probe-log helpers**

In `tools/present_spike/harness.cpp`, add these includes near the top (after the existing includes):

```cpp
#include <sstream>
#include <string>
```

Then add these functions above `main`:

```cpp
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
```

- [ ] **Step 2: Wire `clickthrough` into `main`**

In `tools/present_spike/harness.cpp`, replace the line:

```cpp
    printf("test '%s' not implemented yet\n", test);
    return 0;
```

with:

```cpp
    if (strcmp(test, "clickthrough") == 0) { TestClickthrough(mode, dwmFlush); }
    else { printf("test '%s' not implemented yet\n", test); return 0; }
    printf("%s done; see %%TEMP%%\\present_spike_results.log\n", test);
    return 0;
```

- [ ] **Step 3: Build**

Run: `build.bat spike`
Expected: exit code 0.

- [ ] **Step 4: Run the click-through matrix (probe in background, then each mode)**

Run:
```
start "" clickprobe.exe --seconds 30
harness.exe blt clickthrough
harness.exe dcomp-nolayer clickthrough
harness.exe dcomp-layered clickthrough
type "%TEMP%\present_spike_results.log"
```
Expected: three `clickthrough mode=... result=...` lines.
- `blt` should be `PASS` (sanity: the shipping config passes clicks).
- `dcomp-layered` is expected `PASS` (the #24 finding).
- `dcomp-nolayer` is the open question (`PASS` would be the breakthrough; `FAIL` confirms the Windows constraint).
Record the three results. A `blt` FAIL means the harness/probe setup is wrong (e.g. another window stole the click - run on a clean desktop) - fix before trusting the others.

- [ ] **Step 5: Commit**

```bash
git add tools/present_spike/harness.cpp
git commit -m "spike: real cross-process click-through test (#69)"
```

---

## Task 5: Pacing test

**Files:**
- Modify: `tools/present_spike/harness.cpp` (add `DetectHz` + `pacing` test, wire into `main`)

- [ ] **Step 1: Add `DetectHz` and the pacing test**

In `tools/present_spike/harness.cpp`, add above `main`:

```cpp
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
```

- [ ] **Step 2: Wire `pacing` into `main`**

In `main`, change:

```cpp
    if (strcmp(test, "clickthrough") == 0) { TestClickthrough(mode, dwmFlush); }
    else { printf("test '%s' not implemented yet\n", test); return 0; }
```

to:

```cpp
    if (strcmp(test, "clickthrough") == 0) { TestClickthrough(mode, dwmFlush); }
    else if (strcmp(test, "pacing") == 0)  { TestPacing(mode, dwmFlush); }
    else { printf("test '%s' not implemented yet\n", test); return 0; }
```

- [ ] **Step 3: Build**

Run: `build.bat spike`
Expected: exit code 0.

- [ ] **Step 4: Run the pacing matrix**

Run:
```
harness.exe blt pacing
harness.exe dcomp-nolayer pacing
harness.exe dcomp-layered pacing
type "%TEMP%\present_spike_results.log"
```
Expected: three `pacing mode=...` lines with fps near the display refresh and maxMs/hitch counts. The decision input: do the dcomp modes show lower `maxMs` / fewer `hitch1.5x` than `blt`? (This run is on a static desktop with no game; the definitive numbers come from the user's real-workload run.)

- [ ] **Step 5: Commit**

```bash
git add tools/present_spike/harness.cpp
git commit -m "spike: present pacing test (#69)"
```

---

## Task 6: Latency test + baseline

**Files:**
- Modify: `tools/present_spike/harness.cpp` (add `latency` + `latency-baseline`, wire into `main`)

- [ ] **Step 1: Add the latency test**

In `tools/present_spike/harness.cpp`, add these includes near the top (with the others):

```cpp
#include <vector>
#include <algorithm>
```

Add above `main`:

```cpp
// Measure injection-to-receipt latency for clicks delivered to the probe, with the overlay
// present (overlayPresent=true) or absent (baseline). N samples; reports median + p95.
static void TestLatency(spike::PresentMode mode, bool overlayPresent, bool dwmFlush) {
    int cx, cy;
    if (!ProbeCenter(cx, cy)) {
        spike::LogLine(kResults, "latency mode=%s ERROR no probe rect (run clickprobe.exe first)", ModeName(mode));
        return;
    }
    spike::Overlay ov;
    if (overlayPresent && !ov.init(mode)) { spike::LogLine(kResults, "latency mode=%s ERROR init failed", ModeName(mode)); return; }
    if (overlayPresent) RenderFor(ov, 500, dwmFlush);

    const int N = 30; const long long freq = spike::QpcFreq();
    std::vector<double> lat; MSG m;
    for (int i = 0; i < N; ++i) {
        long long t0 = spike::QpcNow();
        ClickAt(cx, cy);
        long long deadline = t0 + 200LL * freq / 1000;   // give up after 200 ms
        long long recv = 0;
        while (spike::QpcNow() < deadline) {
            while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
            if (overlayPresent) ov.renderFrame((double)i, dwmFlush);
            recv = FirstDownSince(t0);
            if (recv) break;
            Sleep(1);
        }
        if (recv) lat.push_back((double)(recv - t0) / freq * 1000.0);
        Sleep(60);   // gap so clicks don't coalesce
    }
    std::sort(lat.begin(), lat.end());
    double med = lat.empty() ? -1.0 : lat[lat.size() / 2];
    double p95 = lat.empty() ? -1.0 : lat[(size_t)(lat.size() * 0.95)];
    spike::LogLine(kResults, "latency mode=%s overlay=%d samples=%d medianMs=%.2f p95Ms=%.2f",
                   overlayPresent ? ModeName(mode) : "none", overlayPresent ? 1 : 0,
                   (int)lat.size(), med, p95);
    if (overlayPresent) ov.shutdown();
}
```

- [ ] **Step 2: Wire `latency` and `latency-baseline` into `main`**

In `main`, change:

```cpp
    if (strcmp(test, "clickthrough") == 0) { TestClickthrough(mode, dwmFlush); }
    else if (strcmp(test, "pacing") == 0)  { TestPacing(mode, dwmFlush); }
    else { printf("test '%s' not implemented yet\n", test); return 0; }
```

to:

```cpp
    if (strcmp(test, "clickthrough") == 0)          { TestClickthrough(mode, dwmFlush); }
    else if (strcmp(test, "pacing") == 0)           { TestPacing(mode, dwmFlush); }
    else if (strcmp(test, "latency") == 0)          { TestLatency(mode, true, dwmFlush); }
    else if (strcmp(test, "latency-baseline") == 0) { TestLatency(mode, false, false); }
    else { printf("test '%s' not implemented yet\n", test); return 0; }
```

- [ ] **Step 3: Build**

Run: `build.bat spike`
Expected: exit code 0.

- [ ] **Step 4: Run the latency matrix (probe in background)**

Run:
```
start "" clickprobe.exe --seconds 60
harness.exe none latency-baseline
harness.exe blt latency
harness.exe dcomp-nolayer latency
harness.exe dcomp-layered latency
type "%TEMP%\present_spike_results.log"
```
Expected: one `latency mode=none overlay=0 ...` baseline line plus three per-mode lines, each with `medianMs`/`p95Ms`. Decision input: dcomp medians within ~1 frame (~7 ms at 144 Hz) of the `blt` line and not worse than baseline by more than that. (`dcomp-nolayer` may show `samples` near 0 if clicks are eaten - that correlates with a clickthrough FAIL and is itself a result.)

- [ ] **Step 5: Commit**

```bash
git add tools/present_spike/harness.cpp
git commit -m "spike: input latency test + baseline (#69)"
```

---

## Task 7: Runbook + results capture

**Files:**
- Create: `tools/present_spike/README.md`

- [ ] **Step 1: Write the runbook**

Create `tools/present_spike/README.md`:

```markdown
# Present spike (issue #69)

Decide by measurement whether a DirectComposition flip-model overlay beats blt+DwmFlush without
breaking cross-process click-through or regressing background-app responsiveness. See the spec:
`docs/superpowers/specs/2026-05-29-dcomp-present-spike-design.md`.

## Build

    build.bat spike

Produces `clickprobe.exe` and `harness.exe` in the repo root.

## Run (clean desktop, then under the real workload)

Start the click target (a green window near the top-left), then run the harness per config.
Results append to `%TEMP%\present_spike_results.log`.

    start "" clickprobe.exe --seconds 120

    rem click-through (PASS/FAIL)
    harness.exe blt clickthrough
    harness.exe dcomp-nolayer clickthrough
    harness.exe dcomp-layered clickthrough

    rem pacing (avg/max dt, hitch counts)
    harness.exe blt pacing
    harness.exe dcomp-nolayer pacing
    harness.exe dcomp-layered pacing

    rem latency (median/p95 injection-to-receipt)
    harness.exe none latency-baseline
    harness.exe blt latency
    harness.exe dcomp-nolayer latency
    harness.exe dcomp-layered latency

    type "%TEMP%\present_spike_results.log"

## Real-workload run (decides it)

Run the **pacing** and **latency** matrices again with a game running fullscreen-windowed /
borderless on the target monitor, 4K HDR enabled. The background-game responsiveness is the metric
that got the prior attempt (#24) reverted, so its `latency` numbers (overlay present vs baseline)
are the deciding data. Paste the full results log back.

## Decision gate (from the spec)

1. Click-through MUST pass, or the config is dead.
2. A dcomp config must show lower `maxMs` / fewer hitches than `blt` (else no upside).
3. dcomp `medianMs` within ~1 frame (~7 ms @144Hz) of `blt`, and not worse than baseline by more.
4. Subjective confirmation under a real game.

If a dcomp config clears all four, proceed to the Phase 2 engine-migration plan (resurrect
`d93408a`'s `present=dcomp` into `render_engine`). If none does, record the negative result in
CLAUDE.md and close #69.
```

- [ ] **Step 2: Commit**

```bash
git add tools/present_spike/README.md
git commit -m "spike: runbook + decision gate for present spike (#69)"
```

---

## After the plan

Once all tasks are built and the automatable matrices (click-through, pacing, latency on a static
desktop) have run on the dev machine, hand the user the runbook for the real 4K-HDR-over-a-game
measurement. Feed the combined results into the decision gate. Only if a dcomp config clears all
four gates do we write the Phase 2 engine-migration plan; otherwise we close #69 with the negative
result recorded in CLAUDE.md.
