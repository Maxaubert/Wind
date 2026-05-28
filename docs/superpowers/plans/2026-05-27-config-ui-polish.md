# Config UI Polish + Onboarding Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Restyle the Wind config UI into the locked scroll-spy two-tone-rail design with a custom integrated title bar, and add a 3-step first-launch onboarding flow.

**Architecture:** A separate `WindConfig.exe` WebView2 host loads a Svelte SPA that switches between a Settings view and an Onboarding view by launch mode. The host owns a frameless window; the web UI draws the chrome. The core magnifier is untouched except for an `onboarded` flag and a first-launch spawn. All UI-to-core communication stays through `magnifier.ini` (surgical, atomic writes).

**Tech Stack:** C++17/MSVC (host + core), WebView2 (vendored SDK 1.0.2792.45), Svelte 4 + Vite 5, Playwright, doctest.

---

## Conventions for every task

- **No em-dashes anywhere** (project CLAUDE.md rule). Use commas, en-dashes, or rephrase. This includes code comments, UI copy, and commit messages.
- **Kill running binaries before any C++ build** (avoids LNK1104 file lock):
  `powershell -Command "Get-Process Wind,WindConfig -ErrorAction SilentlyContinue | Stop-Process -Force"`
- **The committed HTML mockups are the exact visual + motion reference.** Port markup, CSS, and the SVG animations from them rather than inventing:
  - Settings look: `mockups/config-ui-onepage.html`
  - Onboarding look + wind animation: `mockups/config-ui-onboarding.html`
- Build commands: `build.bat` (Wind.exe), `build.bat test` (doctest, exit 0 = pass), `build.bat config` (npm build of `ui/` then WindConfig.exe).
- Playwright: from `ui/`, `npx playwright test`.

## File structure

**Core (C++):**
- `src/config.h` - add `int onboarded = 0;`.
- `src/config.cpp` - parse `onboarded`; add it to the default-ini writer text.
- `src/main.cpp` - first-launch spawn of `WindConfig.exe --onboard` when `onboarded == 0`.
- `tests/test_config.cpp` - `onboarded` default + parse case.

**Host (C++):**
- `src/config_ui/main.cpp` - frameless custom title bar, `--onboard` mode, `window` + `openIni` bridge messages, a global HWND.

**UI (Svelte), all under `ui/src/`:**
- `bridge.js` - add `getMode`, `windowControl`, `openIni`.
- `theme.css` (new) - dark/light design tokens. `theme.js` (new) - apply/persist `uiTheme`.
- `App.svelte` - becomes a thin router (mode -> Onboarding or Settings).
- `Settings.svelte` (new) - single scrolling page, rail, sections, staged Apply footer.
- `Onboarding.svelte` (new) - 3-step guide, dots, Skip, wind animation, apply-on-advance.
- `lib/Rail.svelte` (new) - icon rail + scroll-spy + theme toggle + cog + avatar.
- `lib/Section.svelte` (new) - sticky section header + rows.
- `lib/KeybindCapture.svelte` (new) - keydown/side-button capture control.
- `lib/scrollspy.js` (new) - Svelte action: click-to-scroll + active-on-scroll.
- `lib/Row.svelte` - add `keybind` and `button` control types.
- `lib/icons.js` (new) - shared inline-SVG strings (ported from the mockups).
- `settings-schema.js` - sections incl. About; keybind + button rows.

**Tests (UI):**
- `ui/tests/settings.spec.js` - extend (render-all, scroll-spy active, theme toggle writes `uiTheme`, staged Apply, keybind capture).
- `ui/tests/onboarding.spec.js` (new) - 3-step flow, apply-on-advance, `onboarded=1`.

---

## Task 1: Core `onboarded` key + first-launch spawn

**Files:**
- Modify: `src/config.h` (Config struct, after `cropCapture`)
- Modify: `src/config.cpp` (parser + default-ini writer)
- Modify: `src/main.cpp` (wWinMain, before the main loop)
- Test: `tests/test_config.cpp`

- [ ] **Step 1: Write the failing test**

Add to `tests/test_config.cpp` (after the `cropCapture can be set` case):

```cpp
TEST_CASE("onboarded defaults to 0 and parses") {
    CHECK(ParseConfig("").onboarded == 0);
    CHECK(ParseConfig("onboarded=1\n").onboarded == 1);
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `build.bat test`
Expected: FAIL to compile (`onboarded` is not a member of `Config`).

- [ ] **Step 3: Add the field**

In `src/config.h`, immediately after the `cropCapture` member (the last field, line ~70):

```cpp
    int    cropCapture = 1;
    // First-launch onboarding: 0 = not yet onboarded (also true of a freshly created ini), so the
    // core spawns WindConfig.exe --onboard once; the onboarding flow sets this to 1 on completion.
    int    onboarded = 0;
```

- [ ] **Step 4: Parse it**

In `src/config.cpp`, in `ParseConfig`'s if/else chain, after the `cropCapture` line:

```cpp
            else if (key == "cropCapture")        c.cropCapture = std::stoi(val);
            else if (key == "onboarded")          c.onboarded = std::stoi(val);
```

- [ ] **Step 5: Add to the default-ini writer**

In `src/config.cpp` `LoadConfig`, in the default-ini `out << ...` block, append after the `cropCapture=1\n` line (keep the closing `;`):

```cpp
               "cropCapture=1\n"
               "; onboarded: 0 = run the first-launch setup once; set to 1 once finished\n"
               "onboarded=0\n";
```

- [ ] **Step 6: Run the test to verify it passes**

Run: `build.bat test`
Expected: PASS, `[doctest] Status: SUCCESS!` (all cases, including the new one).

- [ ] **Step 7: Add the first-launch spawn**

In `src/main.cpp` `wWinMain`, after the two self-test early-return blocks and immediately before `QueryPerformanceFrequency(&ts.freq);` (line ~451), add:

```cpp
    // First launch: open the guided setup once (off the hot path). onboarded==0 also covers a
    // freshly created ini. Non-blocking: spawn WindConfig.exe --onboard and continue to the tray.
    if (cfg.onboarded == 0) {
        wchar_t cmd[] = L"WindConfig.exe --onboard";
        STARTUPINFOW si{}; si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        if (CreateProcessW(L"WindConfig.exe", cmd, nullptr, nullptr, FALSE,
                           0, nullptr, nullptr, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
    }
```

- [ ] **Step 8: Build the app to verify it compiles**

Run (kill first): `powershell -Command "Get-Process Wind,WindConfig -ErrorAction SilentlyContinue | Stop-Process -Force"` then `build.bat`
Expected: `Wind.exe` produced, exit 0.

- [ ] **Step 9: Commit**

```bash
git add src/config.h src/config.cpp src/main.cpp tests/test_config.cpp
git commit -m "feat: onboarded config key + first-launch WindConfig --onboard spawn (#57)"
```

---

## Task 2: Bridge additions (mode, window control, openIni)

**Files:**
- Modify: `ui/src/bridge.js`
- Modify: `ui/tests/settings.spec.js` (mock must accept the new message types so existing tests stay green)

- [ ] **Step 1: Add the bridge functions**

Append to `ui/src/bridge.js` (after `setConfig`):

```js
// Launch mode: WindConfig.exe navigates to ...?mode=onboard for first-launch setup.
export function getMode() {
  return new URLSearchParams(location.search).get('mode') === 'onboard' ? 'onboard' : 'settings';
}
// Custom title bar buttons -> host runs ShowWindow(SW_MINIMIZE) / WM_CLOSE.
export function windowControl(action) { post({ type: 'window', action }); }
// "Edit config file" -> host opens magnifier.ini in the default editor.
export function openIni() { post({ type: 'openIni' }); }
```

- [ ] **Step 2: Make the Playwright mock tolerate the new messages**

In `ui/tests/settings.spec.js`, in the `postMessage` mock, the existing handler ignores unknown types already (it only branches on `getConfig`/`setConfig`), so `window`/`openIni` are no-ops. No change needed here yet; the onboarding spec (Task 8) will assert on them. Verify the existing tests still pass.

- [ ] **Step 3: Build the UI and run Playwright**

Run: `cd ui && npm run build && npx playwright test`
Expected: existing 2 tests PASS.

- [ ] **Step 4: Commit**

```bash
git add ui/src/bridge.js
git commit -m "feat: config bridge getMode/windowControl/openIni (#57)"
```

---

## Task 3: Host - window-control + openIni messages + --onboard mode

**Files:**
- Modify: `src/config_ui/main.cpp`

- [ ] **Step 1: Add a global HWND and shellapi include**

In `src/config_ui/main.cpp`, after the existing includes add `#include <shellapi.h>`, and after the `g_webview` global (line ~14) add:

```cpp
static HWND g_hwnd = nullptr;
```

- [ ] **Step 2: Handle the new message types**

In `HandleWebMessage`, extend the if/else chain after the `setConfig` branch:

```cpp
    } else if (type == "window") {
        std::string action = JsonField(j, "action");
        if (action == "minimize") ShowWindow(g_hwnd, SW_MINIMIZE);
        else if (action == "close") PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
    } else if (type == "openIni") {
        ShellExecuteW(nullptr, L"open", L"notepad.exe", IniPath().c_str(), nullptr, SW_SHOWNORMAL);
    }
```

- [ ] **Step 3: Read `--onboard` and navigate accordingly**

Change the `wWinMain` signature to capture the command line and set `g_hwnd`, and choose the navigate URL. Replace the signature line and add the mode parse near the top of `wWinMain`:

```cpp
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR lpCmdLine, int) {
    bool onboard = lpCmdLine && wcsstr(lpCmdLine, L"--onboard") != nullptr;
```

After `CreateWindowExW(...)` assigns `hwnd`, set the global:

```cpp
    g_hwnd = hwnd;
```

In the controller-created callback, capture `onboard` and pick the URL. Change the lambda capture from `[hwnd, uiDir]` to `[hwnd, uiDir, onboard]` (both the environment and controller lambdas), and replace the `Navigate` call:

```cpp
                    g_webview->Navigate(onboard
                        ? L"https://wind.config/index.html?mode=onboard"
                        : L"https://wind.config/index.html");
```

- [ ] **Step 4: Build the host**

Run (kill first): `powershell -Command "Get-Process Wind,WindConfig -ErrorAction SilentlyContinue | Stop-Process -Force"` then `build.bat config`
Expected: `WindConfig.exe` produced, exit 0.

- [ ] **Step 5: Manual smoke check**

Run: `WindConfig.exe` (opens settings) and `WindConfig.exe --onboard` (will show the same UI for now; mode wiring lands in Task 8). Confirm the window opens and is not crashing.

- [ ] **Step 6: Commit**

```bash
git add src/config_ui/main.cpp
git commit -m "feat: config host window/openIni messages + --onboard launch mode (#57)"
```

---

## Task 4: Host - frameless custom title bar

**Files:**
- Modify: `src/config_ui/main.cpp`

This is the highest-risk task. Primary approach: a frameless window (remove the OS caption via `WM_NCCALCSIZE`) with WebView2 non-client region support so CSS `app-region: drag` handles dragging. `WM_NCHITTEST` handles resize borders. If the installed WebView2 runtime lacks non-client region support, the fallback returns `HTCAPTION` for the left part of the title band (the window-control buttons sit top-right, so leaving the right strip as `HTCLIENT` keeps them clickable).

- [ ] **Step 1: Create the window without a caption**

Replace the `CreateWindowExW` call's style `WS_OVERLAPPEDWINDOW` with the frameless-but-resizable set:

```cpp
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Wind Settings",
        WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, nullptr, nullptr, hInst, nullptr);
```

- [ ] **Step 2: Strip the non-client frame and hit-test borders in WndProc**

In `WndProc`, before the `WM_SIZE` handler, add `WM_NCCALCSIZE` and `WM_NCHITTEST`:

```cpp
    if (m == WM_NCCALCSIZE && w == TRUE) {
        // Remove the standard window frame so the client area spans the whole window (we draw our
        // own title bar in the web UI). When maximized, inset by the frame so content is not clipped
        // off-screen and the taskbar stays reachable.
        if (IsZoomed(h)) {
            UINT dpi = GetDpiForWindow(h); if (!dpi) dpi = 96;
            int fx = GetSystemMetricsForDpi(SM_CXFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
            int fy = GetSystemMetricsForDpi(SM_CYFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
            auto* p = reinterpret_cast<NCCALCSIZE_PARAMS*>(l);
            p->rgrc[0].left += fx; p->rgrc[0].right -= fx;
            p->rgrc[0].top += fy; p->rgrc[0].bottom -= fy;
        }
        return 0;
    }
    if (m == WM_NCHITTEST) {
        // Resize borders (8px DPI-scaled). Drag is handled by WebView2 non-client regions
        // (CSS app-region: drag); fall back to HTCAPTION on the left of the title band if needed.
        UINT dpi = GetDpiForWindow(h); if (!dpi) dpi = 96;
        const int border = MulDiv(8, dpi, 96);
        const int titleH = MulDiv(44, dpi, 96);
        POINT pt{ GET_X_LPARAM(l), GET_Y_LPARAM(l) }; ScreenToClient(h, &pt);
        RECT rc; GetClientRect(h, &rc);
        bool left = pt.x < border, right = pt.x >= rc.right - border;
        bool top = pt.y < border, bottom = pt.y >= rc.bottom - border;
        if (top && left) return HTTOPLEFT;       if (top && right) return HTTOPRIGHT;
        if (bottom && left) return HTBOTTOMLEFT;  if (bottom && right) return HTBOTTOMRIGHT;
        if (left) return HTLEFT;   if (right) return HTRIGHT;
        if (top) return HTTOP;     if (bottom) return HTBOTTOM;
        // Fallback drag region: left ~70% of the title band (buttons are top-right). With non-client
        // region support enabled this is overridden by the web app-region; harmless either way.
        if (pt.y < titleH && pt.x < rc.right - MulDiv(120, dpi, 96)) return HTCAPTION;
        return HTCLIENT;
    }
```

Add `#include <windowsx.h>` (for `GET_X_LPARAM`/`GET_Y_LPARAM`) to the includes.

- [ ] **Step 3: Enable WebView2 non-client region support**

In the controller-created callback, after `g_controller->get_CoreWebView2(&g_webview);` and before the virtual-host mapping, add:

```cpp
                    { ComPtr<ICoreWebView2Settings> s0;
                      if (SUCCEEDED(g_webview->get_Settings(&s0))) {
                          ComPtr<ICoreWebView2Settings9> s9;
                          if (SUCCEEDED(s0.As(&s9)) && s9)
                              s9->put_IsNonClientRegionSupportEnabled(TRUE);
                      } }
```

If `ICoreWebView2Settings9` is not declared in the vendored header, the `.As(&s9)` simply fails at runtime (`s9` stays null) and the `WM_NCHITTEST` HTCAPTION fallback covers dragging. Do not block on it.

- [ ] **Step 4: Build the host**

Run (kill first): `powershell -Command "Get-Process Wind,WindConfig -ErrorAction SilentlyContinue | Stop-Process -Force"` then `build.bat config`
Expected: `WindConfig.exe` produced, exit 0.

- [ ] **Step 5: Manual check**

Run `WindConfig.exe`. Confirm: no OS title bar; the window can be resized from its edges; it can be moved by dragging the top area (works fully after Task 6 adds the `app-region: drag` title bar, or via the HTCAPTION fallback now). Min size still enforced.

- [ ] **Step 6: Commit**

```bash
git add src/config_ui/main.cpp
git commit -m "feat: frameless custom title bar for the config window (#57)"
```

---

## Task 5: UI theme tokens + uiTheme persistence

**Files:**
- Create: `ui/src/theme.css`
- Create: `ui/src/theme.js`
- Modify: `ui/src/main.js` (import theme.css)

- [ ] **Step 1: Create the token stylesheet**

Create `ui/src/theme.css` with the tokens from the mockups (these exact values; light + dark):

```css
:root {
  --bg:#ffffff; --rail:#e9e9ee; --panel-line:rgba(0,0,0,.08);
  --text:#1c1c22; --muted:#6c6c77; --line:#ededf1; --hover:rgba(0,0,0,.05);
  --accent:#5b5bd6; --accent-icon:#5b5bd6; --accent-soft:rgba(91,91,214,.13);
  --track:rgba(0,0,0,.14); --chip:rgba(0,0,0,.04);
}
@media (prefers-color-scheme: dark) {
  :root:not(.force-light) {
    --bg:#0e0e12; --rail:#17171b; --panel-line:rgba(255,255,255,.06);
    --text:#f3f3f6; --muted:#85858f; --line:#1f1f25; --hover:rgba(255,255,255,.07);
    --accent:#5b5bd6; --accent-icon:#9090f2; --accent-soft:rgba(91,91,214,.22);
    --track:rgba(255,255,255,.16); --chip:rgba(255,255,255,.06);
  }
}
:root.force-dark {
  --bg:#0e0e12; --rail:#17171b; --panel-line:rgba(255,255,255,.06);
  --text:#f3f3f6; --muted:#85858f; --line:#1f1f25; --hover:rgba(255,255,255,.07);
  --accent:#5b5bd6; --accent-icon:#9090f2; --accent-soft:rgba(91,91,214,.22);
  --track:rgba(255,255,255,.16); --chip:rgba(255,255,255,.06);
}
:root.force-light {
  --bg:#ffffff; --rail:#e9e9ee; --panel-line:rgba(0,0,0,.08);
  --text:#1c1c22; --muted:#6c6c77; --line:#ededf1; --hover:rgba(0,0,0,.05);
  --accent:#5b5bd6; --accent-icon:#5b5bd6; --accent-soft:rgba(91,91,214,.13);
  --track:rgba(0,0,0,.14); --chip:rgba(0,0,0,.04);
}
html,body{margin:0;height:100%;background:var(--bg);color:var(--text);
  font-family:"Segoe UI Variable","Segoe UI",system-ui,sans-serif;}
:global(input[type=checkbox]),:global(input[type=range]){accent-color:var(--accent);}
```

- [ ] **Step 2: Create the theme helper**

Create `ui/src/theme.js`:

```js
import { setConfig } from './bridge.js';

// uiTheme = 'auto' | 'dark' | 'light'. auto follows prefers-color-scheme; dark/light force a class
// that overrides the media query. Persisted in magnifier.ini (UI-only key; the core ignores it).
export function applyTheme(mode) {
  const c = document.documentElement.classList;
  c.remove('force-dark', 'force-light');
  if (mode === 'dark') c.add('force-dark');
  else if (mode === 'light') c.add('force-light');
}
export function currentTheme(values) {
  return values && values.uiTheme ? values.uiTheme : 'auto';
}
// The sun/moon toggle cycles auto -> dark -> light -> auto and persists.
export function nextTheme(mode) {
  return mode === 'auto' ? 'dark' : mode === 'dark' ? 'light' : 'auto';
}
export function setTheme(mode) { applyTheme(mode); setConfig('uiTheme', mode); }
```

- [ ] **Step 3: Import the stylesheet at the entry**

In `ui/src/main.js`, add at the top:

```js
import './theme.css';
```

- [ ] **Step 4: Build the UI to verify it compiles**

Run: `cd ui && npm run build`
Expected: build succeeds, `ui/dist` produced.

- [ ] **Step 5: Commit**

```bash
git add ui/src/theme.css ui/src/theme.js ui/src/main.js
git commit -m "feat: config UI theme tokens + uiTheme persistence (#57)"
```

---

## Task 6: Settings view (rail + sections + scroll-spy + staged Apply)

**Files:**
- Create: `ui/src/lib/icons.js`, `ui/src/lib/scrollspy.js`, `ui/src/lib/Rail.svelte`, `ui/src/lib/Section.svelte`, `ui/src/Settings.svelte`
- Modify: `ui/src/settings-schema.js`, `ui/src/App.svelte`
- Test: `ui/tests/settings.spec.js`

Port all visual CSS/markup/SVG from `mockups/config-ui-onepage.html` (the rail, two-tone, sticky 24px headers, control styles, footer). The code below provides the structure and logic; match the mockup for styling.

- [ ] **Step 1: Shared icons**

Create `ui/src/lib/icons.js`, exporting the inline-SVG strings used by the rail and steps. Copy the exact `ic` map from `mockups/config-ui-onepage.html` and `mockups/config-ui-onboarding.html` (keys: `zoom, cursor, display, adv, about, glyph, cog, person, sun, moon, min, close`). Example shape:

```js
export const ic = {
  zoom:    '<svg viewBox="0 0 16 16" width="18" height="18" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round"><circle cx="7" cy="7" r="4.3"/><path d="M10.2 10.2 14 14"/></svg>',
  // ...copy the rest verbatim from mockups/config-ui-onepage.html...
};
```

- [ ] **Step 2: Scroll-spy action**

Create `ui/src/lib/scrollspy.js`:

```js
// Svelte action on the scroll container. opts: { sectionIds, onActive }.
// Click-to-scroll is done by the rail (it calls scrollToSection); this watches scroll and reports
// the active section (the last one whose top has passed the 70px band), matching the mockup.
export function scrollspy(node, opts) {
  let { sectionIds, onActive } = opts;
  function onScroll() {
    let cur = sectionIds[0];
    for (const id of sectionIds) {
      const el = node.querySelector('#sec-' + id);
      if (el && el.offsetTop - node.scrollTop <= 70) cur = id;
    }
    onActive(cur);
  }
  node.addEventListener('scroll', onScroll);
  onScroll();
  return {
    update(o) { sectionIds = o.sectionIds; onActive = o.onActive; onScroll(); },
    destroy() { node.removeEventListener('scroll', onScroll); },
  };
}
export function scrollToSection(node, id) {
  const el = node.querySelector('#sec-' + id);
  if (el) node.scrollTo({ top: el.offsetTop - 4, behavior: 'smooth' });
}
```

- [ ] **Step 3: Rail component**

Create `ui/src/lib/Rail.svelte`. Props: `sections` (array of `{id,label,icon}`), `active`, `onSelect(id)`, `theme`, `onToggleTheme`, `onOpenIni`. Markup/CSS ported from the mockup rail (64px, app glyph top, per-section icon buttons with `.active` accent bar + tint + `title` tooltip, bottom theme toggle + cog + avatar). Logic:

```svelte
<script>
  import { ic } from './icons.js';
  export let sections, active, onSelect, theme, onToggleTheme, onOpenIni;
</script>
<aside class="rail">
  <div class="rail-glyph" title="Wind">{@html ic.glyph}</div>
  <div class="rail-nav">
    {#each sections as s}
      <button class="ritem" class:active={s.id === active} title={s.label}
              on:click={() => onSelect(s.id)}>{@html ic[s.icon]}</button>
    {/each}
  </div>
  <div class="rail-spacer"></div>
  <div class="rail-foot">
    <button class="ritem" title="Toggle theme" on:click={onToggleTheme}>{@html theme === 'light' ? ic.moon : ic.sun}</button>
    <button class="ritem" title="Edit config file" on:click={onOpenIni}>{@html ic.cog}</button>
    <div class="avatar" title="Account (coming soon)">{@html ic.person}</div>
  </div>
</aside>
<style>
  /* Port .rail/.rail-glyph/.rail-nav/.ritem/.ritem.active/.rail-foot/.avatar from
     mockups/config-ui-onepage.html (two-tone, accent bar, hover). */
</style>
```

- [ ] **Step 4: Section component**

Create `ui/src/lib/Section.svelte` (sticky 24px header + a `<slot/>` for rows):

```svelte
<script>
  export let id, label, desc = '';
</script>
<div class="sec" id={'sec-' + id}>
  <div class="sec-head"><h2>{label}</h2>{#if desc}<p>{desc}</p>{/if}</div>
  <slot />
</div>
<style>
  /* Port .sec/.sec-head (position:sticky;top:0) from mockups/config-ui-onepage.html;
     header font-size 24px. */
</style>
```

- [ ] **Step 5: Rewrite the schema with sections + descriptions**

Replace `ui/src/settings-schema.js` so each section has `id`, `label`, `icon`, `desc`, and `rows` (keep all existing keys; add the About section; the keybind rows are added in Task 7). Use the mockup section descriptions:

```js
export const sections = [
  { id:'zoom', label:'Zoom', icon:'zoom', desc:'How magnification grows while you hold the zoom button.', rows: [
    { key:'zoomInSpeed',  type:'slider', label:'Zoom-in speed',  desc:'Multiplier (1.0 = default).', min:0.25, max:4, step:0.05, def:1.0 },
    { key:'zoomOutSpeed', type:'slider', label:'Zoom-out speed', desc:'Multiplier (1.0 = default).', min:0.25, max:4, step:0.05, def:1.0 },
    { key:'smoothZoom',   type:'toggle', label:'Smooth zoom',    desc:'Zoom-in eases up to your speed.', def:0 },
    { key:'smoothZoomAccel', type:'slider', label:'Smooth ease-in depth', desc:'Higher = slower start.', min:1, max:8, step:0.5, def:3.0, dependsOn:'smoothZoom' },
    { key:'smoothZoomRamp',  type:'slider', label:'Smooth ramp (s)', desc:'Seconds to reach full speed.', min:0.1, max:3, step:0.1, def:0.6, dependsOn:'smoothZoom' },
    { key:'maxLevel',     type:'slider', label:'Max zoom',       desc:'How far you can zoom.', min:2, max:50, step:1, def:8.0 },
  ]},
  { id:'cursor', label:'Cursor', icon:'cursor', desc:'Pointer movement and visibility while zoomed.', rows: [
    { key:'cursorSensitivity', type:'slider', label:'Cursor speed', desc:'Pan speed multiplier (1.0 = match your mouse).', min:0.25, max:4, step:0.05, def:1.0 },
    { key:'cursorSmoothing',   type:'slider', label:'Pan smoothing', desc:'0 = off, higher = smoother.', min:0, max:0.95, step:0.05, def:0.8 },
    { key:'cursorScaleWithZoom', type:'toggle', label:'Scale cursor with zoom', def:1 },
    { key:'cursorVisibility', type:'select', label:'Cursor visibility', options:['auto','always','never'], def:'auto' },
  ]},
  { id:'display', label:'Display', icon:'display', desc:'Image quality of the magnified view.', rows: [
    { key:'bilinear',    type:'toggle', label:'Smooth scaling', desc:'Bilinear vs crisp pixels.', def:1 },
    { key:'sharpness',   type:'slider', label:'Sharpness', desc:'Crisps the magnified image (0 = off).', min:0, max:1, step:0.05, def:0.0 },
    { key:'brightness',  type:'slider', label:'Brightness', min:0.5, max:1.5, step:0.05, def:1.0 },
    { key:'hdrTonemap',  type:'toggle', label:'HDR tonemap', desc:'HDR10 to SDR when HDR is on.', def:1 },
    { key:'multiMonitor',type:'toggle', label:'Follow cursor monitor', def:1 },
  ]},
  { id:'adv', label:'Advanced', icon:'adv', desc:'Pacing and diagnostics. Defaults are usually best.', rows: [
    { key:'vsync',       type:'toggle', label:'VSync', def:1 },
    { key:'dwmFlush',    type:'toggle', label:'DWM-flush pacing', def:0 },
    { key:'cropCapture', type:'toggle', label:'Crop capture on full repaints', def:1 },
    { key:'diagnostics', type:'toggle', label:'Frametime logging', def:0 },
    { key:'__openIni',   type:'button', label:'Config file', desc:'Open magnifier.ini in your editor.', action:'openIni', btn:'Edit config file' },
  ]},
  { id:'about', label:'About', icon:'about', desc:'', rows: [
    { key:'__about', type:'about' },
  ]},
];
```

- [ ] **Step 6: Settings view (single page + rail + staged Apply)**

Create `ui/src/Settings.svelte`. Carry the staged-apply logic from the current `App.svelte` (values/saved/change/apply/discard/dirty), render every section stacked inside the scroll container with the scrollspy action, render the rail, and wire theme + openIni:

```svelte
<script>
  import { onMount } from 'svelte';
  import { sections } from './settings-schema.js';
  import { getConfig, setConfig, openIni } from './bridge.js';
  import { currentTheme, applyTheme, nextTheme, setTheme } from './theme.js';
  import Rail from './lib/Rail.svelte';
  import Section from './lib/Section.svelte';
  import Row from './lib/Row.svelte';
  import { scrollspy, scrollToSection } from './lib/scrollspy.js';

  let values = {}, saved = {}, active = sections[0].id, theme = 'auto', scroller;
  const railItems = sections.map(s => ({ id: s.id, label: s.label, icon: s.icon }));
  const ids = sections.map(s => s.id);

  onMount(async () => {
    const cfg = await getConfig();
    const v = {};
    for (const s of sections) for (const r of s.rows) if (r.key[0] !== '_') v[r.key] = (r.key in cfg) ? cfg[r.key] : r.def;
    values = v; saved = { ...v };
    theme = currentTheme(cfg); applyTheme(theme);
  });
  function change(key, val) { values = { ...values, [key]: val }; }
  function apply() {
    for (const k of Object.keys(values)) if (String(values[k]) !== String(saved[k])) setConfig(k, values[k]);
    saved = { ...values };
  }
  function discard() { values = { ...saved }; }
  function toggleTheme() { theme = nextTheme(theme); setTheme(theme); }
  $: dirty = Object.keys(values).some(k => String(values[k]) !== String(saved[k]));
</script>
<div class="win">
  <Rail sections={railItems} {active} onSelect={(id) => scrollToSection(scroller, id)}
        {theme} onToggleTheme={toggleTheme} onOpenIni={openIni} />
  <section class="content">
    <div class="caption" style="app-region:drag;-webkit-app-region:drag">
      <span class="ctitle">Wind Settings</span>
      <div class="tbtns" style="app-region:no-drag;-webkit-app-region:no-drag">
        <!-- window-control buttons added in Task 8's shared TitleButtons; for now port from mockup -->
      </div>
    </div>
    <div class="scroll" bind:this={scroller}
         use:scrollspy={{ sectionIds: ids, onActive: (id) => active = id }}>
      {#each sections as s}
        <Section id={s.id} label={s.label} desc={s.desc}>
          {#each s.rows as r}
            <Row row={r} value={values[r.key]}
                 disabled={r.dependsOn && Number(values[r.dependsOn]) !== 1}
                 onChange={(val) => change(r.key, val)} />
          {/each}
        </Section>
      {/each}
    </div>
    <footer>
      <span class="hint">{dirty ? 'Unsaved changes' : 'All changes saved'}</span>
      <button class="btn" on:click={discard} disabled={!dirty}>Discard</button>
      <button class="btn primary" on:click={apply} disabled={!dirty}>Apply</button>
    </footer>
  </section>
</div>
<style>
  /* Port .win/.content/.caption/.ctitle/.tbtns/.scroll/.footer/.btn from
     mockups/config-ui-onepage.html. Set .caption app-region:drag for the custom title bar. */
</style>
```

- [ ] **Step 7: Point App.svelte at Settings for now**

Replace `ui/src/App.svelte` body with a temporary direct render (the router lands in Task 8):

```svelte
<script>
  import Settings from './Settings.svelte';
</script>
<Settings />
```

- [ ] **Step 8: Update the existing Playwright tests for the new layout**

Rewrite `ui/tests/settings.spec.js` to match the single-page rail layout. The mock must now also return `uiTheme`:

```js
import { test, expect } from '@playwright/test';

test.beforeEach(async ({ page }) => {
  await page.addInitScript(() => {
    window.__sets = [];
    const listeners = new Set();
    window.chrome = { webview: {
      addEventListener: (_e, fn) => listeners.add(fn),
      postMessage: (msg) => {
        if (msg.type === 'getConfig')
          listeners.forEach(fn => fn({ data: { type: 'config', values: { zoomInSpeed: '1.2', smoothZoom: '0', uiTheme: 'auto' } } }));
        if (msg.type === 'setConfig') window.__sets.push(msg);
      },
    }};
  });
});

test('renders all sections on one page', async ({ page }) => {
  await page.goto('/');
  await expect(page.getByText('Zoom-in speed')).toBeVisible();
  await expect(page.getByText('Cursor speed')).toBeVisible();
  await expect(page.getByText('Sharpness')).toBeVisible();
  await expect(page.getByRole('heading', { name: 'About' })).toBeVisible();
});

test('rail click scrolls and marks the section active', async ({ page }) => {
  await page.goto('/');
  await page.getByRole('button', { name: 'Display' }).click();
  await expect(page.getByRole('button', { name: 'Display' })).toHaveClass(/active/);
});

test('theme toggle writes uiTheme', async ({ page }) => {
  await page.goto('/');
  await page.getByRole('button', { name: 'Toggle theme' }).click();
  const sets = await page.evaluate(() => window.__sets);
  expect(sets.some(s => s.key === 'uiTheme')).toBeTruthy();
});

test('changes stage until Apply, then setConfig fires', async ({ page }) => {
  await page.goto('/');
  await page.getByText('Smooth zoom', { exact: true }).locator('xpath=../..').getByRole('checkbox').click();
  expect(await page.evaluate(() => window.__sets.filter(s => s.key === 'smoothZoom').length)).toBe(0);
  await page.getByRole('button', { name: 'Apply' }).click();
  const sets = await page.evaluate(() => window.__sets);
  expect(sets.some(s => s.key === 'smoothZoom' && s.value === '1')).toBeTruthy();
});
```

(The rail buttons use the section `label` as their accessible name via the `title` attribute; if Playwright does not match `getByRole('button',{name})` on `title`, add `aria-label={s.label}` to the `.ritem` button in Rail.svelte. Include that aria-label so these tests are stable.)

- [ ] **Step 9: Build + test**

Run: `cd ui && npm run build && npx playwright test`
Expected: all 4 tests PASS.

- [ ] **Step 10: Commit**

```bash
git add ui/src/lib/icons.js ui/src/lib/scrollspy.js ui/src/lib/Rail.svelte ui/src/lib/Section.svelte ui/src/Settings.svelte ui/src/App.svelte ui/src/settings-schema.js ui/tests/settings.spec.js
git commit -m "feat: settings view - scroll-spy two-tone rail, single page, staged Apply (#57)"
```

---

## Task 7: Keybind capture + button/about row types

**Files:**
- Create: `ui/src/lib/KeybindCapture.svelte`
- Modify: `ui/src/lib/Row.svelte`, `ui/src/settings-schema.js`
- Test: `ui/tests/settings.spec.js` (add a keybind case)

The core OR-combines mouse side-buttons and VK keys (see `src/main.cpp`), so a keybind can be either. The capture writes `zoomInButton`/`zoomInVk` (or the out variants). Mouse `button===3` is XBUTTON1 (back) -> our `zoomInButton`/`zoomOutButton` value 1; `button===4` is XBUTTON2 (forward) -> value 2. Key presses write the VK code to `zoomInVk`/`zoomOutVk`.

- [ ] **Step 1: KeybindCapture component**

Create `ui/src/lib/KeybindCapture.svelte`:

```svelte
<script>
  // row.buttonKey = 'zoomInButton'|'zoomOutButton'; row.vkKey = 'zoomInVk'|'zoomOutVk'.
  // value object holds the current { [buttonKey]: '1'|'2'|'0', [vkKey]: '<vk>' }; onChange(key,val).
  export let row, values, onChange, disabled = false;
  let armed = false;
  const VK_NAMES = { 33:'PageUp', 34:'PageDown', 112:'F1', 113:'F2', 145:'ScrollLock' };
  function label() {
    const btn = Number(values[row.buttonKey] || 0);
    if (btn === 2) return 'Mouse button 5';
    if (btn === 1) return 'Mouse button 4';
    const vk = Number(values[row.vkKey] || 0);
    if (vk) return VK_NAMES[vk] || ('Key ' + vk);
    return 'Unbound';
  }
  function onKey(e) {
    if (!armed) return; e.preventDefault();
    onChange(row.vkKey, String(e.keyCode));
    onChange(row.buttonKey, '0');   // switch to keyboard binding
    armed = false;
  }
  function onMouse(e) {
    if (!armed) return;
    if (e.button === 3) { onChange(row.buttonKey, '1'); onChange(row.vkKey, '0'); e.preventDefault(); armed = false; }
    else if (e.button === 4) { onChange(row.buttonKey, '2'); onChange(row.vkKey, '0'); e.preventDefault(); armed = false; }
  }
</script>
<svelte:window on:keydown={onKey} on:mousedown={onMouse} />
<button class="keycap" class:armed {disabled} on:click={() => armed = true}>
  {armed ? 'Press a key or side-button...' : label()}
</button>
<style>
  /* Port .keycap look from mockups/config-ui-onepage.html. .armed = accent outline. */
</style>
```

- [ ] **Step 2: Wire keybind + button + about into Row.svelte**

Extend `ui/src/lib/Row.svelte`. Add `export let values = {};` (keybind needs sibling keys) and branches:

```svelte
{:else if row.type === 'keybind'}
  <KeybindCapture {row} {values} {onChange} {disabled} />
{:else if row.type === 'button'}
  <button class="linkbtn" on:click={() => onChange('__action', row.action)}>{row.btn}</button>
{:else if row.type === 'about'}
  <div class="about">Wind, a fast magnifier. <a href="https://github.com/Maxaubert/Wind" target="_blank" rel="noopener">GitHub</a></div>
```

Import `KeybindCapture` in Row's script. For the `button` type, `onChange('__action', row.action)` lets the parent route actions (Settings maps `openIni` -> `openIni()`).

- [ ] **Step 3: Surface keybinds in the Zoom section + pass values + handle actions**

In `ui/src/settings-schema.js`, prepend two keybind rows to the Zoom section's `rows`:

```js
    { key:'__zoomIn',  type:'keybind', label:'Zoom in',  desc:'Hold to magnify',  buttonKey:'zoomInButton',  vkKey:'zoomInVk' },
    { key:'__zoomOut', type:'keybind', label:'Zoom out', desc:'Hold to zoom back', buttonKey:'zoomOutButton', vkKey:'zoomOutVk' },
```

In `ui/src/Settings.svelte`: (a) load the keybind sibling keys into `values` (extend the onMount loop to also pull `zoomInButton/zoomInVk/zoomOutButton/zoomOutVk` defaults `2/33/1/34`); (b) pass `values` to `Row`; (c) in `change`, intercept the `__action` key:

```js
  function change(key, val) {
    if (key === '__action') { if (val === 'openIni') openIni(); return; }
    values = { ...values, [key]: val };
  }
```

Keybind writes go through `change(buttonKey/vkKey, ...)` so they stage and are written on Apply like everything else.

- [ ] **Step 4: Add a keybind Playwright case**

Append to `ui/tests/settings.spec.js`:

```js
test('keybind capture writes a VK on keydown', async ({ page }) => {
  await page.goto('/');
  await page.getByText('Zoom in', { exact: true }).locator('xpath=../..').getByRole('button').click();
  await page.keyboard.press('F2'); // keyCode 113
  await page.getByRole('button', { name: 'Apply' }).click();
  const sets = await page.evaluate(() => window.__sets);
  expect(sets.some(s => s.key === 'zoomInVk' && s.value === '113')).toBeTruthy();
});
```

(The mock's `getConfig` should also return `zoomInButton:'2', zoomInVk:'33', zoomOutButton:'1', zoomOutVk:'34'` so the control renders; add those keys to the mock's values object.)

- [ ] **Step 5: Build + test**

Run: `cd ui && npm run build && npx playwright test`
Expected: all tests PASS (5 now).

- [ ] **Step 6: Commit**

```bash
git add ui/src/lib/KeybindCapture.svelte ui/src/lib/Row.svelte ui/src/settings-schema.js ui/src/Settings.svelte ui/tests/settings.spec.js
git commit -m "feat: keybind capture + button/about rows in settings (#57)"
```

---

## Task 8: App router + Onboarding view

**Files:**
- Modify: `ui/src/App.svelte`
- Create: `ui/src/Onboarding.svelte`
- Test: `ui/tests/onboarding.spec.js`

Port the 3-step markup, dots, Skip, the custom title bar buttons, and the welcome wind-trails-into-logo animation (SVG + keyframes) verbatim from `mockups/config-ui-onboarding.html`. The router and apply-on-advance logic are below.

- [ ] **Step 1: Router**

Replace `ui/src/App.svelte`:

```svelte
<script>
  import { onMount } from 'svelte';
  import { getMode, getConfig } from './bridge.js';
  import { currentTheme, applyTheme } from './theme.js';
  import Settings from './Settings.svelte';
  import Onboarding from './Onboarding.svelte';
  let mode = getMode();
  onMount(async () => { const cfg = await getConfig(); applyTheme(currentTheme(cfg)); });
  function goToSettings() { mode = 'settings'; }
</script>
{#if mode === 'onboard'}
  <Onboarding onDone={goToSettings} />
{:else}
  <Settings />
{/if}
```

- [ ] **Step 2: Onboarding component (apply-on-advance)**

Create `ui/src/Onboarding.svelte`. Three steps: Welcome (wind animation), Set keys (two `KeybindCapture` rows, staged locally), You're all set (animated check ring). Apply-on-advance: pressing Next on the keys step writes the staged binding keys via `setConfig`; the final "Open Settings" writes `onboarded=1`, calls `onDone`. `windowControl` for min/close.

```svelte
<script>
  import { setConfig, windowControl } from './bridge.js';
  import { ic } from './lib/icons.js';
  import KeybindCapture from './lib/KeybindCapture.svelte';
  export let onDone;
  let cur = 0;
  const N = 3;
  // Staged key bindings (defaults match the core). Written on advancing past the keys step.
  let keys = { zoomInButton:'2', zoomInVk:'33', zoomOutButton:'1', zoomOutVk:'34' };
  function setKey(k, v) { keys = { ...keys, [k]: v }; }
  function applyKeys() { for (const k of Object.keys(keys)) setConfig(k, keys[k]); }
  function next() {
    if (cur === 1) applyKeys();          // leaving "Set your zoom keys" applies them
    if (cur === N - 1) { setConfig('onboarded', '1'); onDone(); return; }
    cur += 1;
  }
  function back() { if (cur > 0) cur -= 1; }
  function skip() { setConfig('onboarded', '1'); onDone(); }
  const zoomInRow  = { label:'Zoom in',  desc:'Hold to magnify',  buttonKey:'zoomInButton',  vkKey:'zoomInVk' };
  const zoomOutRow = { label:'Zoom out', desc:'Hold to zoom back', buttonKey:'zoomOutButton', vkKey:'zoomOutVk' };
</script>
<div class="win">
  <div class="caption" style="app-region:drag;-webkit-app-region:drag">
    <div class="tbtns" style="app-region:no-drag;-webkit-app-region:no-drag">
      <button class="tbtn" title="Minimize" on:click={() => windowControl('minimize')}>{@html ic.min}</button>
      <button class="tbtn close" title="Close" on:click={() => windowControl('close')}>{@html ic.close}</button>
    </div>
  </div>
  <div class="wizbody">
    <!-- Step 0: Welcome (wind-trails-into-logo animation: port the .hero SVG + windsvg/logosvg
         markup and the windTrail/trailsOut/logoIn/logoDraw keyframes from
         mockups/config-ui-onboarding.html; gate with class:show on cur===0) -->
    <div class="step center" class:show={cur === 0}>
      <!-- port hero here -->
      <h2>Welcome to Wind</h2>
      <p>A fast magnifier that lives in your tray. Let's set up the essentials.</p>
    </div>
    <!-- Step 1: Set your zoom keys -->
    <div class="step" class:show={cur === 1}>
      <h2>Set your zoom keys</h2>
      <p>Pick the buttons you'll hold to zoom. Mouse side-buttons work great, or choose keyboard keys.</p>
      <div class="orow"><div class="ot"><div class="rlabel">Zoom in</div><div class="rdesc">Hold to magnify</div></div>
        <div class="rctl"><KeybindCapture row={zoomInRow} values={keys} onChange={setKey} /></div></div>
      <div class="orow"><div class="ot"><div class="rlabel">Zoom out</div><div class="rdesc">Hold to zoom back</div></div>
        <div class="rctl"><KeybindCapture row={zoomOutRow} values={keys} onChange={setKey} /></div></div>
    </div>
    <!-- Step 2: You're all set (animated check ring; port .bigring + ring/tick keyframes from mockup) -->
    <div class="step center" class:show={cur === 2}>
      <!-- port check ring here -->
      <h2>You're all set</h2>
    </div>
  </div>
  <div class="wizdots">{#each Array(N) as _, i}<i class:on={i === cur}></i>{/each}</div>
  <div class="wizfoot">
    {#if cur < N - 1}<button class="skip" on:click={skip}>Skip setup</button>{/if}
    {#if cur > 0}<button class="btn" on:click={back}>Back</button>{/if}
    <button class="btn primary" on:click={next}>{cur === 0 ? 'Get started' : cur === N - 1 ? 'Open Settings' : 'Next'}</button>
  </div>
</div>
<style>
  /* Port .win/.caption/.tbtn/.wizbody/.step/.step.show/.orow/.wizdots/.wizfoot/.skip/.btn and the
     welcome animation + check-ring keyframes from mockups/config-ui-onboarding.html. The .step.show
     gating drives the welcome animation replay and the check-ring draw. */
</style>
```

- [ ] **Step 3: Onboarding Playwright test**

Create `ui/tests/onboarding.spec.js`:

```js
import { test, expect } from '@playwright/test';

test.beforeEach(async ({ page }) => {
  await page.addInitScript(() => {
    window.__sets = [];
    const listeners = new Set();
    window.chrome = { webview: {
      addEventListener: (_e, fn) => listeners.add(fn),
      postMessage: (msg) => {
        if (msg.type === 'getConfig')
          listeners.forEach(fn => fn({ data: { type: 'config', values: { uiTheme: 'auto' } } }));
        if (msg.type === 'setConfig') window.__sets.push(msg);
      },
    }};
  });
});

test('onboarding walks 3 steps, applies keys on advance, sets onboarded', async ({ page }) => {
  await page.goto('/?mode=onboard');
  await expect(page.getByRole('heading', { name: 'Welcome to Wind' })).toBeVisible();
  await page.getByRole('button', { name: 'Get started' }).click();
  await expect(page.getByRole('heading', { name: 'Set your zoom keys' })).toBeVisible();
  await page.getByRole('button', { name: 'Next' }).click();        // applies keys
  expect(await page.evaluate(() => window.__sets.some(s => s.key === 'zoomInButton'))).toBeTruthy();
  await expect(page.getByRole('heading', { name: "You're all set" })).toBeVisible();
  await page.getByRole('button', { name: 'Open Settings' }).click();
  expect(await page.evaluate(() => window.__sets.some(s => s.key === 'onboarded' && s.value === '1'))).toBeTruthy();
});

test('settings mode does not show onboarding', async ({ page }) => {
  await page.goto('/');
  await expect(page.getByText('Zoom-in speed')).toBeVisible();
});
```

- [ ] **Step 4: Build + test**

Run: `cd ui && npm run build && npx playwright test`
Expected: all specs PASS (settings + onboarding).

- [ ] **Step 5: Commit**

```bash
git add ui/src/App.svelte ui/src/Onboarding.svelte ui/tests/onboarding.spec.js
git commit -m "feat: app router + 3-step onboarding (wind intro, apply-on-advance) (#57)"
```

---

## Task 9: Full build, integration verification, manual checklist

**Files:** none (verification only)

- [ ] **Step 1: Kill running binaries, full build**

Run: `powershell -Command "Get-Process Wind,WindConfig -ErrorAction SilentlyContinue | Stop-Process -Force"`
Run: `build.bat test` (expect SUCCESS), `build.bat` (expect `Wind.exe`), `build.bat config` (expect ui build + `WindConfig.exe`).

- [ ] **Step 2: Playwright full run**

Run: `cd ui && npx playwright test`
Expected: all specs PASS.

- [ ] **Step 3: Manual checklist** (run the built binaries)

- `WindConfig.exe`: frameless window, draggable by the title strip, minimize + close work, resizable, min size enforced; rail scroll-spy (click + scroll), sticky 24px headers, staged Apply writes on Apply only; theme toggle flips dark/light and persists (reopen to confirm `uiTheme` stuck); keybind capture rebinds zoom in/out; "Edit config file" opens the ini.
- `WindConfig.exe --onboard`: 3-step flow; welcome wind-into-logo animation; Next on the keys step applies them (verify by holding the key in another app while onboarding is open, since the core hot-reloads); "Open Settings" switches to the settings view and the window keeps running.
- First-launch: set `onboarded=0` in `magnifier.ini`, launch `Wind.exe`; confirm `WindConfig.exe --onboard` auto-opens and the magnifier still runs in the tray. Finish onboarding, confirm `onboarded=1` is written and it does not reopen on the next `Wind.exe` launch.
- Confirm zoom still works and feels unchanged (the core path is untouched).

- [ ] **Step 4: Final review + branch finish**

Dispatch a final code review across the branch, then use superpowers:finishing-a-development-branch to open the PR against `main` referencing #57.

---

## Self-review (against the spec)

**Spec coverage:**
- Custom title bar -> Task 4 (frameless) + Task 6/8 (web app-region drag + window buttons). Covered.
- Single scrolling page + scroll-spy rail -> Task 6. Covered.
- Staged Apply kept -> Task 6 (ported from current App.svelte). Covered.
- Theme tokens + uiTheme persist (auto/dark/light) -> Task 5 + Task 6 toggle. Covered.
- Keybind capture (VK + side-button) -> Task 7. Covered.
- About + account placeholder -> Task 6 schema (`about` row) + Rail avatar. Covered.
- Onboarding 3-step, apply-on-advance, no auto-apply/Apply button, Open Settings -> Task 8. Covered.
- Welcome wind-into-logo animation -> Task 8 (ported from mockup). Covered.
- onboarded key + first-launch spawn -> Task 1. Covered.
- `window`/`openIni` bridge + mode -> Tasks 2/3. Covered.
- Tests: Playwright (settings scroll-spy/theme/staged Apply/keybind, onboarding flow) + test_config onboarded -> Tasks 1, 6, 7, 8. Covered.

**Placeholder scan:** Visual CSS/SVG is delegated to the committed mockups by explicit file reference (not a TODO); all logic, signatures, and tests are spelled out. No "TBD/handle edge cases" left in code steps.

**Type/name consistency:** `getMode/windowControl/openIni` (bridge) used consistently; `uiTheme` key consistent across theme.js, Settings, tests; keybind row props `buttonKey/vkKey` consistent across schema, Row, KeybindCapture, Onboarding; `__action`/`openIni` routing consistent; section objects use `{id,label,icon,desc,rows}` consistently in schema, Rail, Settings, Section.

**Risk note:** Task 4 (frameless title bar) is the main risk; the WebView2 non-client region path has a `WM_NCHITTEST` HTCAPTION fallback so dragging works even without runtime support.
