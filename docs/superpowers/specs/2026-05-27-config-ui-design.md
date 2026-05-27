# Wind Config UI - Design

**Branch:** `feat/config-ui` (stacked on `feat/zoom-config` so the settings schema matches the
current config keys; rebase onto `main` after that merges).
**Status:** approved (design), ready for implementation plan.

## Goal

Give real users a clean GUI to adjust Wind's settings, plus a guided first-launch onboarding -
without touching the magnifier's performance. The app still starts to the tray; the user opens the
config from there. On first launch the config opens automatically as a guided onboarding flow
(set a keybind + a couple of feel prefs), distinct from the normal settings. The look should adapt
to OS light/dark, be easy to restyle and extend, and leave room to add a user DB / licensing later.

## Locked decisions (from brainstorming)

1. **Separate process**, communicating only via `magnifier.ini` (which the core already hot-reloads
   via its dir-watch). The zoom core is never touched by UI work -> zero perf coupling.
2. **Web frontend (HTML/CSS/JS) in a thin C++ WebView2 host** - stays in the existing C++/MSVC
   toolchain; no bundled browser runtime.
3. **Separate `WindConfig.exe`** (not a `Wind.exe --config` mode) - keeps the perf-critical core
   lean and free of the WebView2 dependency, and doesn't complicate the core's UIAccess signing.
4. **Keep `magnifier.ini`** as the shared format. The host does **surgical** key read/writes that
   preserve comments, order, and hand-edits. Core parser unchanged.
5. **Svelte + Vite** frontend (tiny compiled output, good structure as it grows). Adds a Node build
   step for the UI only.
6. **Onboarding = guided essentials:** Welcome -> set zoom keybind -> zoom-speed + smooth-zoom ->
   "you're set (lives in the tray)".

## Phasing

**MVP (this plan):** functional settings end to end - `WindConfig.exe` host + bridge, the surgical
ini module, a schema-driven Svelte settings screen with **live-apply**, the `build.bat config`
target, and a tray **"Open Settings"** entry that launches it. Styling is clean-but-basic.

**Later (separate plans):** the guided **onboarding** flow + first-launch auto-spawn (`onboarded`
key), and the **visual polish** (full Tabby-style theming, light/dark tokens, sidebar sections).
The MVP is built so these slot in without rework (schema-driven UI, mode flag reserved).

## Architecture & data flow

```
Wind.exe (core, perf-critical)            WindConfig.exe (new, on-demand)
  - magnifier + tray                        - Win32 window hosting WebView2
  - hot-reloads magnifier.ini (dir-watch)   - loads built Svelte UI
  - first launch: spawns WindConfig --onboard - JS<->C++ bridge
  - tray "Open Settings": spawns WindConfig - surgical read/write of magnifier.ini
                \                              /
                 \                            /
                  v                          v
                      magnifier.ini  (the ONLY shared contract)
```

The UI writes a key -> the core's dir-watch fires -> the core reloads and applies it live (the user
sees it by zooming). No IPC, no protocol between the two processes. Writes are **atomic**
(write temp file, then `MoveFileEx` replace) so the core never reads a half-written file.

## `WindConfig.exe` - the C++ host

Thin and single-purpose:

- Creates a borderless-or-standard Win32 window, initializes the WebView2 Evergreen runtime
  (`CreateCoreWebView2EnvironmentWithOptions`).
- Serves the built Svelte assets from a folder next to the exe via
  `ICoreWebView2::SetVirtualHostNameToFolderMapping` (e.g. `https://wind.config/` ->
  `./ui/`), and navigates to `https://wind.config/index.html`. (Virtual-host mapping avoids
  `file://` SPA quirks.)
- Runs a small **message bridge**. JS posts JSON messages (`window.chrome.webview.postMessage`);
  the host handles them and replies via `PostWebMessageAsJson`:
  - `{type:"getConfig"}` -> `{type:"config", values:{key:value,...}}` (parsed from the ini; missing
    keys fall back to the core's defaults, which the host also knows).
  - `{type:"setConfig", key, value}` -> surgical ini update (see below), then ack.
  - `{type:"completeOnboarding"}` -> set `onboarded=1`.
  - `{type:"openIniInEditor"}` -> launch the ini in the default editor (power-user escape).
  - `{type:"close"}` -> close the window.
- Keybind capture happens in JS (see frontend); the host just persists the resulting value.
- Launched with `--onboard` to start in the onboarding flow; otherwise normal settings. The host
  forwards this mode to the UI via a query param or an initial message.

### Surgical ini read/write (host, unit-tested pure logic)

A small module (mirrors the style/testability of `src/config.cpp`'s pure half):
- **Read:** parse `key=value` lines into a map (reuse the same trim/comment rules as the core).
- **Write `setValue(key, value)`:** load the file's lines; if a non-comment line matches `key=`,
  replace its value in place (keep the rest of the line/section/comments); else append `key=value`
  at the end. Write to a temp file in the same dir, then atomically replace the original.
- Preserves comments, ordering, and unknown keys (hand-edits survive). Never rewrites the whole file
  from a template.

## Svelte frontend

A single Svelte+Vite app with two entry flows selected by the launch mode:

### Settings (normal mode) - Tabby-style
- **Left sidebar:** sections - **Zoom**, **Cursor**, **Display**, **Advanced**, **About**.
- **Main panel:** grouped rows, each = label + short description + control. Control types:
  toggle, slider (with a live numeric readout), number input, dropdown, and a **keybind capture**.
- **Live apply:** every control change calls `setConfig` immediately (no Save button); the core
  hot-reloads and the change is live. A **"Reset to defaults"** affordance (per-section or global).
- Section -> setting mapping (initial):
  - **Zoom:** keybind(s), `zoomInSpeed`, `zoomOutSpeed`, `smoothZoom`, `smoothZoomAccel`,
    `smoothZoomRamp`, `maxLevel`.
  - **Cursor:** `cursorSensitivity`, `cursorSmoothing`, `cursorScaleWithZoom`, `cursorVisibility`.
  - **Display:** `bilinear`, `brightness`, `hdrTonemap`, `multiMonitor`.
  - **Advanced:** `vsync`, `dwmFlush`, `cropCapture`, `diagnostics`, `zorderBand`, "Edit config file".
  - **About:** version, links (GitHub etc.), placeholder for a future account/licensing area.

### Onboarding (`--onboard`) - guided essentials
A centered card with steps: **Welcome** -> **Set zoom keybind** (capture a key or a mouse
side-button) -> **Feel** (`zoomInSpeed`/`zoomOutSpeed` slider + `smoothZoom` toggle) -> **Done**
("Wind lives in your tray - open Settings any time"). Writes live as you go; **Finish** sends
`completeOnboarding` and closes.

### Schema-driven
A single `settings-schema` (one entry per setting: `iniKey`, `type`, `label`, `description`,
`section`, and `min`/`max`/`step` or `options`) drives both the rendering and the value mapping.
Adding a setting = one schema entry; adding a section = a sidebar entry. This keeps the UI and the
ini keys in sync and makes the UI trivial to extend.

### Keybind capture
A capture control that, while focused, listens for `keydown` (-> Virtual-Key code, for
`zoomInVk`/`zoomOutVk`) and `pointerdown`/`mousedown` with `event.button === 3/4` (-> XBUTTON1/2,
for `zoomInButton`/`zoomOutButton`). Shows the captured binding; writes the matching ini key. Lets
the user pick a mouse side-button (the default) or a keyboard key.

## Theming

CSS custom properties (design tokens) in one file; `@media (prefers-color-scheme: dark/light)`
switches the token set automatically. Tabby-like visuals: accent-colored active sidebar item,
rounded toggles, generous spacing, section headers. Restyling = edit the tokens file.

## First-launch detection & tray (core changes)

- Add `onboarded` (int, default `0`) to `Config` + the default-ini text (the only core-side addition
  besides spawning).
- `Wind.exe` startup (off the hot path): if `onboarded == 0` (covers a freshly created ini too),
  `ShellExecute`/`CreateProcess` `WindConfig.exe --onboard`, then continue to the tray as normal.
  The onboarding flow sets `onboarded=1` on finish, so it never auto-opens again.
- Tray menu: **Open Settings** (spawn `WindConfig.exe`), keep **Edit config file** (opens the ini in
  the editor) for power users, **Quit**. (Replaces today's notepad-only "Edit config".)
- The core never blocks on or waits for the UI; it just spawns it and moves on.

## Build & packaging

- New `ui/` folder: Svelte + Vite project. `npm install` + `npm run build` -> `ui/dist/`.
- `WindConfig.exe` built by MSVC (a new target in `build.bat`, e.g. `build.bat config`), linking
  the WebView2 loader (`WebView2LoaderStatic.lib` or the NuGet/SDK headers vendored under
  `third_party/`).
- Shipping layout: `Wind.exe`, `WindConfig.exe`, and the built UI assets in `./ui/` beside them.
- WebView2 runtime: rely on the **Evergreen** runtime (preinstalled on Windows 11; a bootstrapper
  can be added for older Win10 if needed - out of scope for v1).
- Dev loop: load `ui/dist` (rebuild on change), or point the host at the Vite dev server during
  development.

## Performance isolation (the hard requirement)

`WindConfig.exe` is a separate, normal-priority process that exists only while open. It shares
nothing with the core except the ini file, and writes it atomically. The core's render loop,
threads, pacing, and hot-reload are untouched. There is no measurable path by which the config UI
can affect zoom performance.

## Extensibility / future licensing

- New setting = one schema entry; new section = one sidebar entry + panel. The host bridge already
  exposes generic get/set.
- A future **account / licensing** screen is just another section/flow in the Svelte app; the host
  can gain a network call or talk to a license server. License tokens / user data live in their own
  store (e.g. a separate file or the OS credential store), **not** in `magnifier.ini`. The
  separate-process web architecture is well suited to an account UI.

## Testing

- **Host surgical ini module:** pure unit tests (doctest, like `tests/test_config.cpp`) - update an
  existing key in place, append a missing key, preserve comments / order / unknown keys, and a
  read-modify-write round-trip. Atomic-replace verified by writing and re-reading.
- **UI (Playwright E2E)** against the built Svelte app with a **mock bridge** (a stub that records
  `setConfig` calls and serves `getConfig`): settings render from a schema, toggling a control emits
  the correct `setConfig{key,value}`, the onboarding flow steps through and emits
  `completeOnboarding`, and light/dark render correctly via the emulated color scheme. (Matches the
  project rule to prefer Playwright for UI.)
- **Manual:** first-run auto-onboarding, tray "Open Settings", and live-apply (adjust a value, then
  zoom to see it change).

## File structure (new)

- `src/config_ui/`: the C++ host - `main.cpp` (window + WebView2 + bridge),
  `ini_edit.{h,cpp}` (pure surgical read/write, unit-tested). Built to `WindConfig.exe`.
- `ui/`: Svelte + Vite app - `src/` (App, Settings, Onboarding, components, `settings-schema.ts`,
  theme tokens), `package.json`, `vite.config.*`.
- `tests/test_ini_edit.cpp`: host ini-module unit tests.
- `ui/tests/`: Playwright specs.
- `build.bat`: a `config` target for `WindConfig.exe`.
- Core: `src/config.{h,cpp}` gains `onboarded`; `src/main.cpp` gains the first-launch spawn;
  `src/tray.cpp` gains "Open Settings".

## Out of scope (v1)

- Actual login / licensing / user DB (architecture leaves room; not built now).
- Config sync across machines.
- A WebView2 bootstrapper for old Windows 10 (rely on Evergreen for now).
- In-window live magnifier preview (the live-apply + zoom-to-see loop covers it).
