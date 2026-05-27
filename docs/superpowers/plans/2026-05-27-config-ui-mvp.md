# Wind Config UI (MVP) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A working settings GUI: a separate `WindConfig.exe` (C++ + WebView2) hosting a Svelte UI that reads/writes `magnifier.ini`, opened from the tray, applying changes live - with onboarding and visual polish deferred.

**Architecture:** `WindConfig.exe` is a standalone process that talks to the core only through `magnifier.ini` (the core already hot-reloads it), so the zoom path is untouched. A thin C++ WebView2 host loads the built Svelte app from disk via a virtual host mapping and exposes a tiny `getConfig`/`setConfig` JSON bridge; `setConfig` does a surgical, atomic ini write. The Svelte app renders controls from a schema and live-applies on change.

**Tech Stack:** C++17/MSVC (host), WebView2 (Evergreen runtime + vendored SDK), Svelte + Vite + Node (UI), doctest (host unit tests), Playwright (UI E2E).

**Spec:** `docs/superpowers/specs/2026-05-27-config-ui-design.md`
**Branch:** `feat/config-ui` (already created).

---

## File structure

- `third_party/webview2/` - vendored WebView2 SDK (`include/WebView2.h` etc. + `x64/WebView2LoaderStatic.lib`).
- `src/config_ui/ini_edit.h` / `ini_edit.cpp` - **pure** surgical ini read/update (no `<windows.h>`; unit-tested).
- `src/config_ui/main.cpp` - the Win32 + WebView2 host (`WindConfig.exe`): window, WebView2 init, virtual-host mapping, the JSON bridge, and the Win32 file read / atomic write.
- `tests/test_ini_edit.cpp` - doctest unit tests for `ini_edit`.
- `ui/` - Svelte + Vite app: `src/settings-schema.js`, `src/bridge.js`, `src/App.svelte`, `src/lib/*.svelte`, `index.html`, `package.json`, `vite.config.js`; builds to `ui/dist/`.
- `ui/tests/settings.spec.js` + `ui/playwright.config.js` - Playwright E2E with a mock bridge.
- `build.bat` - add a `config` target (build UI + compile the host) and add `ini_edit.cpp` to the `test` target.
- `src/tray.cpp` - add an "Open Settings" menu item that launches `WindConfig.exe`.

The core's perf path (`render_engine`, the tick loop) is **not** touched.

---

### Task 1: Vendor the WebView2 SDK

**Files:** Create `third_party/webview2/` (headers + x64 static loader).

- [ ] **Step 1: Download + extract the SDK** (PowerShell, from repo root)

```powershell
$ver = "1.0.2792.45"
$nupkg = "$env:TEMP\webview2.zip"
Invoke-WebRequest "https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/$ver" -OutFile $nupkg
$dst = "$env:TEMP\wv2"; Remove-Item $dst -Recurse -Force -ErrorAction SilentlyContinue
Expand-Archive $nupkg -DestinationPath $dst
New-Item -ItemType Directory -Force third_party\webview2\include, third_party\webview2\x64 | Out-Null
Copy-Item "$dst\build\native\include\*" third_party\webview2\include\ -Recurse -Force
Copy-Item "$dst\build\native\x64\WebView2LoaderStatic.lib" third_party\webview2\x64\ -Force
```

- [ ] **Step 2: Verify the files exist**

Run: `dir third_party\webview2\include\WebView2.h third_party\webview2\x64\WebView2LoaderStatic.lib`
Expected: both files listed. (If the NuGet layout differs by version, locate `WebView2.h` and `WebView2LoaderStatic.lib` under the extracted `$dst` and copy them to the same destinations.)

- [ ] **Step 3: Record the version** - create `third_party/webview2/VERSION.txt` containing the line `Microsoft.Web.WebView2 1.0.2792.45`.

- [ ] **Step 4: Commit**

```bash
git add third_party/webview2
git commit -m "chore: vendor WebView2 SDK (headers + x64 static loader)"
```

---

### Task 2: Pure ini_edit module + unit tests (TDD)

**Files:**
- Create: `src/config_ui/ini_edit.h`, `src/config_ui/ini_edit.cpp`
- Test: `tests/test_ini_edit.cpp`
- Modify: `build.bat` (the `:test` compile line)

- [ ] **Step 1: Write the failing tests** - create `tests/test_ini_edit.cpp`:

```cpp
#include "doctest.h"
#include "../src/config_ui/ini_edit.h"
using namespace wind;

TEST_CASE("ReadIniValues parses key=value, skipping comments and blanks") {
    auto m = ReadIniValues("; c\nmaxLevel=8.0\n\nzoomInSpeed = 1.2\n# x\n");
    CHECK(m["maxLevel"] == "8.0");
    CHECK(m["zoomInSpeed"] == "1.2");        // trimmed
    CHECK(m.count("c") == 0);
}
TEST_CASE("UpdateIniText replaces an existing key in place, preserving the rest") {
    std::string t = "; speed knob\nzoomInSpeed=1.0\nmaxLevel=8.0\n";
    std::string r = UpdateIniText(t, "zoomInSpeed", "2.0");
    auto m = ReadIniValues(r);
    CHECK(m["zoomInSpeed"] == "2.0");
    CHECK(m["maxLevel"] == "8.0");                       // untouched
    CHECK(r.find("; speed knob") != std::string::npos);  // comment preserved
}
TEST_CASE("UpdateIniText appends a missing key") {
    std::string r = UpdateIniText("maxLevel=8.0\n", "smoothZoom", "1");
    auto m = ReadIniValues(r);
    CHECK(m["smoothZoom"] == "1");
    CHECK(m["maxLevel"] == "8.0");
}
TEST_CASE("UpdateIniText leaves unknown keys and comment-only lines intact") {
    std::string t = "; header\nfoo=bar\n; mid\nzoomInSpeed=1.0\n";
    std::string r = UpdateIniText(t, "zoomInSpeed", "3.0");
    CHECK(r.find("foo=bar") != std::string::npos);
    CHECK(r.find("; mid") != std::string::npos);
    CHECK(ReadIniValues(r)["zoomInSpeed"] == "3.0");
}
TEST_CASE("read-modify-write round trip is stable") {
    std::string r = UpdateIniText(UpdateIniText("a=1\nb=2\n", "a", "10"), "c", "3");
    auto m = ReadIniValues(r);
    CHECK(m["a"] == "10"); CHECK(m["b"] == "2"); CHECK(m["c"] == "3");
}
```

- [ ] **Step 2: Add `ini_edit.cpp` to the test build** - in `build.bat`, on the `:test` `cl` line, append `src\config_ui\ini_edit.cpp` to the source list (after `src\lock_detector.cpp`):

```
   src\transform.cpp src\zoom_controller.cpp src\config.cpp src\cursor_mapper.cpp src\lock_detector.cpp src\config_ui\ini_edit.cpp ^
```

- [ ] **Step 3: Run tests to verify they fail**

Run: `build.bat test`
Expected: compile error - `ini_edit.h` not found / `ReadIniValues` undefined. That is the failing state.

- [ ] **Step 4: Create `src/config_ui/ini_edit.h`**

```cpp
#pragma once
#include <string>
#include <map>
namespace wind {
// Parse INI text into key->value, skipping ';'/'#' comments and blank lines; keys/values trimmed.
std::map<std::string, std::string> ReadIniValues(const std::string& text);
// Return INI text with `key`'s value replaced IN PLACE, preserving every other line (comments,
// order, unknown keys). If `key` is absent, append "key=value". Pure (no I/O, no <windows.h>).
std::string UpdateIniText(const std::string& text, const std::string& key, const std::string& value);
}
```

- [ ] **Step 5: Create `src/config_ui/ini_edit.cpp`**

```cpp
#include "ini_edit.h"
#include <sstream>
namespace wind {
static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}
std::map<std::string, std::string> ReadIniValues(const std::string& text) {
    std::map<std::string, std::string> out;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        std::string t = trim(line);
        if (t.empty() || t[0] == ';' || t[0] == '#') continue;
        size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim(t.substr(0, eq));
        if (!k.empty()) out[k] = trim(t.substr(eq + 1));
    }
    return out;
}
std::string UpdateIniText(const std::string& text, const std::string& key, const std::string& value) {
    std::istringstream in(text);
    std::string line, out;
    bool replaced = false;
    const bool endsWithNewline = !text.empty() && text.back() == '\n';
    while (std::getline(in, line)) {
        if (!replaced) {
            std::string t = trim(line);
            const bool comment = t.empty() || t[0] == ';' || t[0] == '#';
            size_t eq = t.find('=');
            if (!comment && eq != std::string::npos && trim(t.substr(0, eq)) == key) {
                out += key + "=" + value + "\n";
                replaced = true;
                continue;
            }
        }
        out += line + "\n";
    }
    if (!replaced) out += key + "=" + value + "\n";
    if (!endsWithNewline && !out.empty() && out.back() == '\n') out.pop_back();
    return out;
}
}
```

- [ ] **Step 6: Run tests to verify they pass**

Run: `build.bat test`
Expected: PASS - all cases, "Status: SUCCESS!".

- [ ] **Step 7: Commit**

```bash
git add src/config_ui/ini_edit.h src/config_ui/ini_edit.cpp tests/test_ini_edit.cpp build.bat
git commit -m "feat: pure surgical ini read/update module + unit tests"
```

---

### Task 3: WebView2 host shell (window + load a placeholder UI)

**Files:**
- Create: `src/config_ui/main.cpp`, `ui/dist/index.html` (temporary placeholder, replaced in Task 5)
- Modify: `build.bat` (add `:config` target)

This task stands up `WindConfig.exe` opening a window that displays local web content. The bridge comes in Task 4; the real Svelte build replaces the placeholder in Task 5.

- [ ] **Step 1: Create a placeholder `ui/dist/index.html`**

```html
<!doctype html><html><head><meta charset="utf-8"><title>Wind Settings</title></head>
<body style="font-family:sans-serif"><h1>Wind Settings</h1><p>WebView2 host OK.</p></body></html>
```

- [ ] **Step 2: Create `src/config_ui/main.cpp`** (uses `Microsoft::WRL::Callback` for the async handlers; links the static loader)

```cpp
#include <windows.h>
#include <wrl.h>
#include <wil/com.h>            // if unavailable, use Microsoft::WRL::ComPtr instead (see note)
#include "WebView2.h"
#include <string>
#include <shlwapi.h>
#pragma comment(lib, "shlwapi.lib")

using namespace Microsoft::WRL;
static wil::com_ptr<ICoreWebView2Controller> g_controller;
static wil::com_ptr<ICoreWebView2> g_webview;
// Stub for Task 3 (so the host links + runs with a placeholder UI); Task 4 replaces the body.
static void HandleWebMessage(ICoreWebView2* /*wv*/, const std::wstring& /*json*/) {}

// Directory of this exe (the UI assets sit in <exeDir>\ui).
static std::wstring ExeDir() {
    wchar_t p[MAX_PATH]; GetModuleFileNameW(nullptr, p, MAX_PATH);
    PathRemoveFileSpecW(p); return p;
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_SIZE && g_controller) { RECT r; GetClientRect(h, &r); g_controller->put_Bounds(r); return 0; }
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(h, m, w, l);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
    WNDCLASSW wc{}; wc.lpfnWndProc = WndProc; wc.hInstance = hInst; wc.lpszClassName = L"WindConfigWnd";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Wind Settings", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 960, 680, nullptr, nullptr, hInst, nullptr);
    ShowWindow(hwnd, SW_SHOW);

    std::wstring uiDir = ExeDir() + L"\\ui\\dist";   // the built Vite output (Task 5); placeholder for Task 3
    CreateCoreWebView2EnvironmentWithOptions(nullptr, nullptr, nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
        [hwnd, uiDir](HRESULT, ICoreWebView2Environment* env) -> HRESULT {
            env->CreateCoreWebView2Controller(hwnd,
                Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                [hwnd, uiDir](HRESULT, ICoreWebView2Controller* controller) -> HRESULT {
                    g_controller = controller;
                    g_controller->get_CoreWebView2(&g_webview);
                    RECT r; GetClientRect(hwnd, &r); g_controller->put_Bounds(r);
                    g_webview->SetVirtualHostNameToFolderMapping(
                        L"wind.config", uiDir.c_str(),
                        COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
                    EventRegistrationToken tok;
                    g_webview->add_WebMessageReceived(
                        Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                        [](ICoreWebView2* wv, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                            LPWSTR json = nullptr;
                            if (SUCCEEDED(args->get_WebMessageAsJson(&json)) && json) {
                                HandleWebMessage(wv, json); CoTaskMemFree(json);
                            }
                            return S_OK;
                        }).Get(), &tok);
                    g_webview->Navigate(L"https://wind.config/index.html");
                    return S_OK;
                }).Get());
            return S_OK;
        }).Get());

    MSG msg; while (GetMessageW(&msg, nullptr, 0, 0)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    return 0;
}
```

NOTE for the implementer: if `wil/com.h` is not vendored, replace `wil::com_ptr<T>` with `Microsoft::WRL::ComPtr<T>` (already used elsewhere in this repo) and use `.Get()`/`&` accordingly. `Callback<>` comes from `<wrl.h>`. Iterate until it compiles and links against `third_party/webview2`.

- [ ] **Step 3: Add the `config` target to `build.bat`** (after the `:uiaccess` block, before `:test`)

```
rem --- Config UI host (WindConfig.exe). Builds the Svelte UI first if Node is present. ----
:config
if exist "%ROOT%ui\package.json" (
  pushd "%ROOT%ui"
  if not exist node_modules ( call npm install || (popd & echo [build] npm install failed & exit /b 1) )
  call npm run build || (popd & echo [build] ui build failed & exit /b 1)
  popd
)
cl /nologo /std:c++17 /EHsc /O2 /W4 /DUNICODE /D_UNICODE ^
   /I third_party\webview2\include ^
   src\config_ui\main.cpp src\config_ui\ini_edit.cpp ^
   /Fe:WindConfig.exe ^
   /link third_party\webview2\x64\WebView2LoaderStatic.lib ^
   user32.lib shell32.lib shlwapi.lib ole32.lib version.lib /SUBSYSTEM:WINDOWS
exit /b %errorlevel%
```

The host maps the virtual host directly to `<exeDir>\ui\dist`, so no asset staging/copy is needed - the Vite build writes `ui/dist` in place and the host reads it there.

- [ ] **Step 4: Build and run**

Run: `build.bat config` then `.\WindConfig.exe`
Expected: a 960x680 window opens showing "Wind Settings / WebView2 host OK." Close it. (If WebView2 runtime is missing, install the Evergreen runtime.)

- [ ] **Step 5: Commit**

```bash
git add src/config_ui/main.cpp ui/dist/index.html build.bat
git commit -m "feat: WindConfig.exe WebView2 host shell loading local UI"
```

---

### Task 4: The config bridge (getConfig / setConfig)

**Files:** Modify `src/config_ui/main.cpp` (add `HandleWebMessage` + Win32 file read / atomic write).

- [ ] **Step 1: Add file I/O + the message handler to `main.cpp`** (append; uses `ini_edit` + the core's `magnifier.ini` resolved next to the exe)

```cpp
#include "ini_edit.h"
#include <fstream>
#include <sstream>

static std::wstring IniPath() { return ExeDir() + L"\\magnifier.ini"; }

static std::string ReadFileUtf8(const std::wstring& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return "";
    std::stringstream ss; ss << f.rdbuf(); return ss.str();
}
// Atomic: write a temp file in the same dir, then replace the target.
static void WriteFileAtomic(const std::wstring& path, const std::string& text) {
    std::wstring tmp = path + L".tmp";
    { std::ofstream f(tmp, std::ios::binary | std::ios::trunc); f.write(text.data(), (std::streamsize)text.size()); }
    MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

static std::wstring Widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0'); MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n); return w;
}
static std::string Narrow(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0'); WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr); return s;
}
// Minimal JSON string-field extractor (the messages are tiny + we control them).
static std::string JsonField(const std::string& j, const std::string& key) {
    size_t k = j.find("\"" + key + "\""); if (k == std::string::npos) return "";
    size_t c = j.find(':', k); if (c == std::string::npos) return "";
    size_t q1 = j.find('"', c + 1); if (q1 == std::string::npos) return "";
    size_t q2 = j.find('"', q1 + 1); if (q2 == std::string::npos) return "";
    return j.substr(q1 + 1, q2 - q1 - 1);
}
static std::string JsonEscape(const std::string& s) {
    std::string o; for (char ch : s) { if (ch == '"' || ch == '\\') o += '\\'; o += ch; } return o;
}

void HandleWebMessage(ICoreWebView2* wv, const std::wstring& jsonW) {
    std::string j = Narrow(jsonW);
    std::string type = JsonField(j, "type");
    if (type == "getConfig") {
        auto vals = wind::ReadIniValues(ReadFileUtf8(IniPath()));
        std::string out = "{\"type\":\"config\",\"values\":{";
        bool first = true;
        for (auto& kv : vals) { if (!first) out += ","; first = false;
            out += "\"" + JsonEscape(kv.first) + "\":\"" + JsonEscape(kv.second) + "\""; }
        out += "}}";
        wv->PostWebMessageAsJson(Widen(out).c_str());
    } else if (type == "setConfig") {
        std::string key = JsonField(j, "key"), value = JsonField(j, "value");
        if (!key.empty()) WriteFileAtomic(IniPath(), wind::UpdateIniText(ReadFileUtf8(IniPath()), key, value));
    }
}
```

NOTE: remove the `extern void HandleWebMessage(...)` forward declaration's `extern` if you define it in the same file; keep one definition. Values are passed as strings both ways; the Svelte schema knows each key's type.

- [ ] **Step 2: Build**

Run: `build.bat config`
Expected: exit 0, `WindConfig.exe` builds. (Behavior is exercised by the Svelte app in Task 5 + Playwright in Task 7.)

- [ ] **Step 3: Commit**

```bash
git add src/config_ui/main.cpp
git commit -m "feat: config bridge - getConfig + surgical atomic setConfig"
```

---

### Task 5: Svelte + Vite settings UI

**Files:** Create the `ui/` project (`package.json`, `vite.config.js`, `index.html`, `src/main.js`, `src/App.svelte`, `src/settings-schema.js`, `src/bridge.js`, `src/lib/Row.svelte`).

- [ ] **Step 1: Scaffold** (PowerShell, repo root)

```powershell
New-Item -ItemType Directory -Force ui\src\lib | Out-Null
```

Create `ui/package.json`:

```json
{
  "name": "wind-config-ui",
  "private": true,
  "type": "module",
  "scripts": { "dev": "vite", "build": "vite build", "test": "playwright test" },
  "devDependencies": {
    "@sveltejs/vite-plugin-svelte": "^3.1.0",
    "svelte": "^4.2.0",
    "vite": "^5.2.0",
    "@playwright/test": "^1.44.0"
  }
}
```

Create `ui/vite.config.js`:

```js
import { defineConfig } from 'vite';
import { svelte } from '@sveltejs/vite-plugin-svelte';
export default defineConfig({ plugins: [svelte()], base: './', build: { outDir: 'dist' } });
```

Create `ui/index.html`:

```html
<!doctype html><html><head><meta charset="utf-8" /><title>Wind Settings</title></head>
<body><div id="app"></div><script type="module" src="/src/main.js"></script></body></html>
```

Create `ui/src/main.js`:

```js
import App from './App.svelte';
export default new App({ target: document.getElementById('app') });
```

- [ ] **Step 2: The bridge client** - create `ui/src/bridge.js` (real WebView2 bridge with a browser/Playwright mock fallback):

```js
const wv = window.chrome && window.chrome.webview;
const listeners = new Set();
if (wv) wv.addEventListener('message', e => listeners.forEach(fn => fn(e.data)));

export function onMessage(fn) { listeners.add(fn); return () => listeners.delete(fn); }
export function post(msg) {
  if (wv) wv.postMessage(msg);
  else window.__windMock && window.__windMock(msg);   // Playwright/browser mock
}
export function getConfig() {
  return new Promise(resolve => {
    const off = onMessage(m => { if (m && m.type === 'config') { off(); resolve(m.values || {}); } });
    post({ type: 'getConfig' });
  });
}
export function setConfig(key, value) { post({ type: 'setConfig', key, value: String(value) }); }
```

- [ ] **Step 3: The settings schema** - create `ui/src/settings-schema.js` (matches the current config keys; `def` = default used when the ini lacks the key):

```js
export const sections = [
  { id: 'zoom', label: 'Zoom', rows: [
    { key: 'zoomInSpeed',  type: 'slider', label: 'Zoom-in speed',  desc: 'Multiplier (1.0 = default).', min: 0.25, max: 4, step: 0.05, def: 1.0 },
    { key: 'zoomOutSpeed', type: 'slider', label: 'Zoom-out speed', desc: 'Multiplier (1.0 = default).', min: 0.25, max: 4, step: 0.05, def: 1.0 },
    { key: 'smoothZoom',   type: 'toggle', label: 'Smooth zoom',    desc: 'Zoom-in eases up to your speed.', def: 0 },
    { key: 'smoothZoomAccel', type: 'slider', label: 'Smooth ease-in depth', desc: 'Higher = slower start.', min: 1, max: 8, step: 0.5, def: 3.0 },
    { key: 'smoothZoomRamp',  type: 'slider', label: 'Smooth ramp (s)', desc: 'Seconds to reach full speed.', min: 0.1, max: 3, step: 0.1, def: 0.6 },
    { key: 'maxLevel',     type: 'slider', label: 'Max zoom',       desc: 'How far you can zoom.', min: 2, max: 50, step: 1, def: 8.0 },
  ]},
  { id: 'cursor', label: 'Cursor', rows: [
    { key: 'cursorSensitivity', type: 'slider', label: 'Locked sensitivity', desc: 'Pan scale when a game locks the cursor.', min: 0.25, max: 4, step: 0.05, def: 1.0 },
    { key: 'cursorSmoothing',   type: 'slider', label: 'Pan smoothing', desc: '0 = off, higher = smoother.', min: 0, max: 0.95, step: 0.05, def: 0.8 },
    { key: 'cursorScaleWithZoom', type: 'toggle', label: 'Scale cursor with zoom', def: 1 },
    { key: 'cursorVisibility', type: 'select', label: 'Cursor visibility', options: ['auto','always','never'], def: 'auto' },
  ]},
  { id: 'display', label: 'Display', rows: [
    { key: 'bilinear',    type: 'toggle', label: 'Smooth scaling', desc: 'Bilinear vs crisp pixels.', def: 1 },
    { key: 'brightness',  type: 'slider', label: 'Brightness', min: 0.5, max: 1.5, step: 0.05, def: 1.0 },
    { key: 'hdrTonemap',  type: 'toggle', label: 'HDR tonemap', desc: 'HDR10 -> SDR when HDR is on.', def: 1 },
    { key: 'multiMonitor',type: 'toggle', label: 'Follow cursor monitor', def: 1 },
  ]},
  { id: 'advanced', label: 'Advanced', rows: [
    { key: 'vsync',       type: 'toggle', label: 'VSync', def: 1 },
    { key: 'dwmFlush',    type: 'toggle', label: 'DWM-flush pacing', def: 0 },
    { key: 'cropCapture', type: 'toggle', label: 'Crop capture on full repaints', def: 1 },
    { key: 'diagnostics', type: 'toggle', label: 'Frametime logging', def: 0 },
  ]},
];
```

- [ ] **Step 4: The Row component** - create `ui/src/lib/Row.svelte`:

```svelte
<script>
  export let row, value, onChange;
  const num = v => Number(v);
</script>
<div class="row">
  <div class="meta"><div class="label">{row.label}</div>{#if row.desc}<div class="desc">{row.desc}</div>{/if}</div>
  <div class="ctl">
    {#if row.type === 'toggle'}
      <input type="checkbox" checked={num(value) === 1} on:change={e => onChange(e.target.checked ? 1 : 0)} />
    {:else if row.type === 'slider'}
      <input type="range" min={row.min} max={row.max} step={row.step} value={value}
             on:input={e => onChange(e.target.value)} />
      <span class="val">{value}</span>
    {:else if row.type === 'select'}
      <select value={value} on:change={e => onChange(e.target.value)}>
        {#each row.options as o}<option value={o}>{o}</option>{/each}
      </select>
    {/if}
  </div>
</div>
<style>
  .row{display:flex;justify-content:space-between;align-items:center;padding:14px 0;border-bottom:1px solid var(--border)}
  .label{font-weight:600} .desc{font-size:.85em;color:var(--muted)} .val{margin-left:8px;min-width:3ch}
</style>
```

- [ ] **Step 5: The App** - create `ui/src/App.svelte`:

```svelte
<script>
  import { onMount } from 'svelte';
  import { sections } from './settings-schema.js';
  import { getConfig, setConfig } from './bridge.js';
  import Row from './lib/Row.svelte';
  let active = sections[0].id;
  let values = {};
  onMount(async () => {
    const cfg = await getConfig();
    const v = {};
    for (const s of sections) for (const r of s.rows) v[r.key] = (r.key in cfg) ? cfg[r.key] : r.def;
    values = v;
  });
  function change(key, val) { values = { ...values, [key]: val }; setConfig(key, val); }
  $: section = sections.find(s => s.id === active);
</script>
<div class="app">
  <nav>{#each sections as s}<button class:on={s.id === active} on:click={() => active = s.id}>{s.label}</button>{/each}</nav>
  <main>
    <h1>{section.label}</h1>
    {#each section.rows as r}<Row row={r} value={values[r.key]} onChange={v => change(r.key, v)} />{/each}
  </main>
</div>
<style>
  :root{--bg:#fff;--fg:#111;--muted:#666;--border:#e5e5e5;--accent:#5b5bd6;--side:#f5f5f7}
  @media (prefers-color-scheme: dark){:root{--bg:#1a1a1a;--fg:#eee;--muted:#999;--border:#333;--side:#222}}
  :global(body){margin:0;background:var(--bg);color:var(--fg);font-family:system-ui,sans-serif}
  .app{display:flex;height:100vh}
  nav{width:200px;background:var(--side);padding:16px 8px;display:flex;flex-direction:column;gap:4px}
  nav button{text-align:left;padding:10px 12px;border:0;border-radius:8px;background:transparent;color:var(--fg);cursor:pointer;font-size:1em}
  nav button.on{background:var(--accent);color:#fff}
  main{flex:1;padding:24px 32px;overflow:auto}
</style>
```

- [ ] **Step 6: Build the UI**

Run: `cd ui && npm install && npm run build && cd ..`
Expected: `ui/dist/` produced (index.html + assets). Delete the Task 3 placeholder `ui/dist/index.html` first if it conflicts (the Vite build overwrites `dist`).

- [ ] **Step 7: Build the host + run end to end**

Run: `build.bat config` then `.\WindConfig.exe`
Expected: the window shows the sidebar + Zoom settings; moving a slider / toggling writes the matching key to `magnifier.ini` (verify by opening the ini). If `Wind.exe` is running, the change applies live (zoom to see).

- [ ] **Step 8: Commit**

```bash
git add ui/package.json ui/vite.config.js ui/index.html ui/src
git commit -m "feat: Svelte settings UI - schema-driven, live-apply via the bridge"
```

---

### Task 6: Tray "Open Settings"

**Files:** Modify `src/tray.cpp`.

- [ ] **Step 1: Add the menu item + handler** - in `src/tray.cpp`:
  - Add an id near the others: `static const UINT ID_SETTINGS = 1003;`
  - In the popup menu, insert before "Edit config":
    `AppendMenuW(m, MF_STRING, ID_SETTINGS, L"Open Settings");`
  - In the command handling, add:

```cpp
        if (cmd == ID_SETTINGS)
            ShellExecuteW(nullptr, L"open", L"WindConfig.exe", nullptr, nullptr, SW_SHOW);
        else if (cmd == ID_EDIT)
            ShellExecuteW(nullptr, L"open", L"notepad.exe", L"magnifier.ini", nullptr, SW_SHOW);
```

(Keep the existing `ID_EDIT`/`ID_QUIT` behavior; just add the `ID_SETTINGS` branch.)

- [ ] **Step 2: Build the core**

Run: `build.bat`
Expected: exit 0, `Wind.exe` builds.

- [ ] **Step 3: Manual check**

Run `.\Wind.exe`, right-click the tray icon -> "Open Settings" -> `WindConfig.exe` opens. (Both binaries must be in the same folder.)

- [ ] **Step 4: Commit**

```bash
git add src/tray.cpp
git commit -m "feat: tray 'Open Settings' launches WindConfig.exe"
```

---

### Task 7: Playwright E2E with a mock bridge

**Files:** Create `ui/playwright.config.js`, `ui/tests/settings.spec.js`.

- [ ] **Step 1: Playwright config** - create `ui/playwright.config.js`:

```js
import { defineConfig } from '@playwright/test';
export default defineConfig({
  testDir: './tests',
  webServer: { command: 'npm run dev', port: 5173, reuseExistingServer: true },
  use: { baseURL: 'http://localhost:5173' },
});
```

- [ ] **Step 2: The spec** - create `ui/tests/settings.spec.js` (injects a mock bridge before the app loads, asserting render + that a toggle emits the right setConfig):

```js
import { test, expect } from '@playwright/test';

test.beforeEach(async ({ page }) => {
  await page.addInitScript(() => {
    window.__sets = [];
    // Mock the WebView2 bridge: capture posts, reply to getConfig synchronously.
    const listeners = new Set();
    window.chrome = { webview: {
      addEventListener: (_e, fn) => listeners.add(fn),
      postMessage: (msg) => {
        if (msg.type === 'getConfig')
          listeners.forEach(fn => fn({ data: { type: 'config', values: { zoomInSpeed: '1.2', smoothZoom: '0' } } }));
        if (msg.type === 'setConfig') window.__sets.push(msg);
      },
    }};
  });
});

test('renders settings and reflects ini values', async ({ page }) => {
  await page.goto('/');
  await expect(page.getByText('Zoom')).toBeVisible();
  await expect(page.getByText('Zoom-in speed')).toBeVisible();
});

test('toggling a setting emits setConfig with the right key', async ({ page }) => {
  await page.goto('/');
  await page.getByText('Smooth zoom').locator('xpath=../..').getByRole('checkbox').click();
  const sets = await page.evaluate(() => window.__sets);
  expect(sets.some(s => s.key === 'smoothZoom' && s.value === '1')).toBeTruthy();
});
```

- [ ] **Step 3: Install browsers + run**

Run: `cd ui && npx playwright install chromium && npx playwright test && cd ..`
Expected: both tests pass.

- [ ] **Step 4: Commit**

```bash
git add ui/playwright.config.js ui/tests
git commit -m "test: Playwright E2E for the settings UI (mock bridge)"
```

---

## Manual verification (after all tasks)

1. `build.bat` (core) + `build.bat config` (UI host) both succeed; `Wind.exe`, `WindConfig.exe`, and `ui/` assets sit together.
2. Run `Wind.exe`; tray -> "Open Settings" opens the UI.
3. Change zoom-in speed / toggle smooth zoom; confirm `magnifier.ini` updates (comments preserved) and, with the magnifier zoomed, the change applies live.
4. Switch the OS theme light<->dark; reopen the UI; it adapts.

## Notes for the implementer

- The host file I/O is intentionally in `main.cpp` (Win32) so `ini_edit.cpp` stays pure and unit-testable (it is compiled into BOTH `WindConfig.exe` and the `build.bat test` binary).
- Keep `WindConfig.exe` a separate process; do not link it into `Wind.exe` and do not touch `render_engine`/the tick loop.
- WebView2 specifics (handler signatures, `wil` vs `WRL::ComPtr`) may need small iteration against the vendored `WebView2.h`; the success criterion is "WindConfig.exe builds, opens, shows the UI, and read/writes the ini." Use `Microsoft::WRL::ComPtr` (already in this repo) if `wil` is not vendored.
- Deferred (later plans, do NOT build now): onboarding flow + `--onboard`, first-launch auto-spawn + `onboarded` key, full Tabby-style theming/polish.
