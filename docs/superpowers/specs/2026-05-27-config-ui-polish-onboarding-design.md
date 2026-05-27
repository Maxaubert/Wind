# Wind Config UI - Visual Polish + Onboarding Design

**Issue:** #57
**Branch:** `feat/config-ui-polish` (off `main`, builds on the merged MVP #53 / #55).
**Status:** design, pending user review.

## Goal

Turn the functional config UI MVP into the locked visual direction, and add the guided
first-launch onboarding. No changes to the magnifier core's performance path (still a separate
`WindConfig.exe` process talking only through `magnifier.ini`).

## Relationship to the MVP

The MVP (merged) already ships: `WindConfig.exe` WebView2 host, the surgical `ini_edit` module,
a schema-driven Svelte settings screen with a staged Apply/Discard footer, per-monitor DPI, a
hardcoded `#5b5bd6` accent, light/dark tokens, and a tray "Open Settings" entry. This phase
restyles that screen, adds the scroll-spy icon rail and the custom title bar, and adds the
onboarding flow plus first-launch auto-spawn. The staged-Apply settings model is kept.

## Visual source of truth (mockups)

The agreed look is captured in committed, throwaway HTML mockups (open in a browser):

- `mockups/config-ui-onepage.html` - the settings window (single scrolling page, scroll-spy rail,
  dark + light, theme toggle).
- `mockups/config-ui-onboarding.html` - the 3-step onboarding (wind-trails-into-logo intro, set
  keys, you're all set).
- `mockups/config-ui-rail.html`, `mockups/config-ui-mockups.html` - earlier exploration kept for
  reference.

Implementation should match these mockups; they are the canonical reference for layout, spacing,
colors, and motion.

## Settings window

### Window chrome (custom integrated title bar)

Frameless window: no OS caption strip. The web content draws the chrome so it flows into the app.

- Host: create the window without the standard caption. Use `WS_POPUP | WS_THICKFRAME |
  WS_MINIMIZEBOX | WS_MAXIMIZEBOX` and handle `WM_NCCALCSIZE` to extend the client area over the
  whole window (no system title bar), while `WS_THICKFRAME` keeps native resize. Handle
  `WM_NCHITTEST` for the resize borders.
- Dragging: prefer WebView2 non-client region support (`ICoreWebView2Settings9::
  put_IsNonClientRegionSupportEnabled(TRUE)` plus CSS `app-region: drag` on the title bar element).
  If the installed runtime lacks it, fall back to the host returning `HTCAPTION` for the top
  title-bar band in `WM_NCHITTEST` (excluding the window-control buttons).
- Window controls: the minimize and close buttons are drawn in the web UI (top-right of the
  content column, per the mockup). Clicking them posts a `window` bridge message and the host runs
  `ShowWindow(SW_MINIMIZE)` or `PostMessage(WM_CLOSE)`. No maximize button on this window.
- Keep the existing per-monitor-V2 DPI awareness, DPI-scaled default size, center-on-work-area,
  and `WM_GETMINMAXINFO` minimum size.

### Layout: single scrolling page + scroll-spy rail

- One scrolling content column holds every section stacked in order: Zoom, Cursor, Display,
  Advanced, About. Section headers are sticky to the top of the scroll area, 24px, with the
  section description beneath.
- Left rail (the two-tone icon rail), 64px wide:
  - App glyph (the Wind mark) at the top in the accent color.
  - One icon per section, vertically stacked. The active section's icon gets an accent-tinted
    rounded background plus a 3px accent bar on the rail's left edge. Hover gives a subtle
    background. Each icon has a tooltip (its section name).
  - Bottom of the rail: a theme toggle (sun/moon), a cog ("Edit config file", opens the ini in the
    editor via a bridge message), and a round account avatar marked "Account (coming soon)" - the
    reserved hook for future licensing.
- Scroll-spy behavior: clicking a rail icon smooth-scrolls the content to that section. As the user
  scrolls, the active rail icon updates to the section currently at the top of the viewport
  (the rail icons are anchors / references, not page swaps).

### Controls and sections

Schema-driven, as in the MVP (`ui/src/settings-schema.js`). Control types: toggle, slider (with a
numeric readout), select, a button/link row, and a new keybind-capture control. Section to setting
mapping stays as today's schema, with keybinds surfaced in Zoom:

- Zoom: zoom-in / zoom-out keybind (capture), `zoomInSpeed`, `zoomOutSpeed`, `smoothZoom`,
  `smoothZoomAccel`, `smoothZoomRamp`, `maxLevel`.
- Cursor: `cursorSensitivity`, `cursorSmoothing`, `cursorScaleWithZoom`, `cursorVisibility`.
- Display: `bilinear`, `sharpness`, `brightness`, `hdrTonemap`, `multiMonitor`.
- Advanced: `vsync`, `dwmFlush`, `cropCapture`, `diagnostics`, plus an "Edit config file" button.
- About: app name + version, a GitHub link, and the account placeholder card.

### Apply model (settings)

Keep the MVP's staged model: control changes stage locally; the footer shows
"Unsaved changes / All changes saved" with Discard and Apply; Apply writes the dirty keys via
`setConfig` (which the core hot-reloads). This is unchanged from the merged MVP.

### Keybind capture control

A control that, while armed (focused/clicked), listens for:

- `keydown` -> a Virtual-Key code, written to `zoomInVk` / `zoomOutVk`.
- `mousedown` with `button === 3` or `4` -> XBUTTON1 / XBUTTON2, written to
  `zoomInButton` / `zoomOutButton`.

It shows the current binding (for example "Mouse button 5" or "PageUp"). Because the core
OR-combines the mouse side-buttons and the VK keys, a binding can be either; the control writes
whichever input type the user pressed and displays it. (No change to the core input model.)

### Theming

CSS custom-property tokens in one place, dark and light sets (values per the mockups: dark uses a
lifted-grey rail `#17171b` over near-black content `#0e0e12`; light uses a soft-grey rail over
white). Accent is `#5b5bd6` everywhere (hardcoded, not the OS accent).

Theme selection persists via a UI-only ini key `uiTheme` = `auto` | `dark` | `light`
(default `auto`). `auto` follows `prefers-color-scheme`; `dark`/`light` force a root class that
overrides the media query. The sun/moon toggle writes `uiTheme` through `setConfig`. The core never
reads `uiTheme`; the surgical ini editor preserves it like any other key.

## Onboarding flow (first launch)

A separate full-page guided flow (no rail), shown only on first launch. Three steps with a dot
progress indicator, the custom title bar (close + minimize), and a "Skip setup" link.

1. **Welcome to Wind** - subtext "A fast magnifier that lives in your tray. Let's set up the
   essentials." Hero animation (see below). Primary button "Get started".
2. **Set your zoom keys** - subtext "Pick the buttons you'll hold to zoom. Mouse side-buttons work
   great, or choose keyboard keys." Two keybind-capture rows (zoom in, zoom out). Primary button
   "Next" applies the keys (see apply model). Back available.
3. **You're all set** - an animated check ring, the title only (no subtext). Primary button
   "Open Settings" switches the same window into the settings view and marks onboarding complete.

### Welcome hero animation

Per `mockups/config-ui-onboarding.html`:

- Five curved wind streamlines sweep right-to-left as comet-tail dashes (an SVG gradient fades each
  trail in and out), staggered for an organic gust feel, for about one second.
- The trails ease out while the actual Wind logo draws in over the top, stroking on in the same
  right-to-left direction (so the motion carries through), decelerating to a settle. No scale pop.
- It then holds static (just the logo). The intro replays whenever the Welcome step is shown again.

### Apply model (onboarding): apply-on-advance, no auto-apply

Onboarding does NOT auto-apply on drag and has no Apply button. Instead, pressing "Next" on a
configure step commits that step's settings via `setConfig` immediately, then advances. The next
screen is therefore live, so the user can hold their keys and feel the result (the core is already
running and hot-reloads the ini). In the 3-step flow only step 2 configures (the keys), so its Next
writes `zoomInButton`/`zoomInVk` and `zoomOutButton`/`zoomOutVk`. (Speed and max are tuned later in
the main Settings, not in onboarding.) The principle generalizes if steps are added.

### First-launch detection and completion

- Add `onboarded` (int, default `0`) to the core `Config` and the default-ini text.
- Core `main.cpp` startup, off the hot path: after `LoadConfig`, if `onboarded == 0` (also covers a
  freshly created ini), `CreateProcess` `WindConfig.exe --onboard`, then continue to the tray. The
  core never blocks on the UI.
- Reaching "Open Settings" posts `setConfig('onboarded', '1')` (so it never auto-opens again) and
  the SPA switches to the settings view in the same window. No relaunch needed.
- Tray "Open Settings" launches `WindConfig.exe` with no `--onboard`, opening straight to settings.

### Launch mode and view switching

- `WindConfig.exe` parses its command line. With `--onboard` it navigates to
  `https://wind.config/index.html?mode=onboard`; otherwise to the plain settings URL.
- The Svelte app reads `location.search` and renders the Onboarding view or the Settings view.
- "Open Settings" performs a client-side switch to the Settings view (same window, no navigation).
- Optional nicety (not required): the host may launch the onboarding window slightly smaller and
  resize/recenter to the settings preset on completion, via a bridge message. If skipped, the
  onboarding card simply centers within the standard window size.

## Architecture mapping onto existing code

### Host (`src/config_ui/main.cpp`)

- Switch window creation to the frameless custom-titlebar approach (`WM_NCCALCSIZE` /
  `WM_NCHITTEST`, optionally `IsNonClientRegionSupportEnabled`).
- Parse the command line for `--onboard`; choose the navigate URL accordingly.
- Extend `HandleWebMessage` with:
  - `window` (action `minimize` | `close`) -> `ShowWindow` / `PostMessage(WM_CLOSE)`.
  - `openIni` -> open `magnifier.ini` in the default editor (`ShellExecute`).
  - (existing `getConfig` / `setConfig` unchanged; `setConfig('onboarded','1')` completes
    onboarding through the generic path, so no new message is required for that).

### Bridge (`ui/src/bridge.js`)

- Add `windowControl(action)` and `openIni()` posting the matching messages, and `getMode()`
  reading `location.search`. Keep the `window.__windMock` fallback so Playwright and a plain
  browser still work.

### Svelte app (`ui/src/`)

- `App.svelte` becomes a thin router: read mode, render `<Onboarding>` or `<Settings>`.
- New components: `Rail.svelte` (icon rail + scroll-spy + theme toggle + cog + avatar),
  `Section.svelte`, a scroll-spy action, `KeybindCapture.svelte`, `Onboarding.svelte` (steps, dots,
  the wind hero animation, apply-on-advance), and a `theme` tokens stylesheet.
- Extend `Row.svelte` / the schema with the `keybind` and `button` control types.
- `Settings.svelte` keeps the staged Apply/Discard footer from the MVP `App.svelte`.

### Core (`src/config.{h,cpp}`, `src/main.cpp`)

- Add `onboarded` to `Config` (default 0), parse it, and include it in the default-ini text.
- `main.cpp`: first-launch spawn of `WindConfig.exe --onboard` when `onboarded == 0`.
- Tray "Open Settings" already exists (from the MVP); no change beyond confirming it launches
  without `--onboard`.

## Performance isolation (unchanged)

`WindConfig.exe` remains a separate, on-demand process sharing only `magnifier.ini` (atomic
writes). The render loop, threads, pacing, and hot-reload are untouched. There is no path by which
the config UI affects zoom performance.

## Testing

- **Playwright E2E** (against the built UI with the mock bridge):
  - Settings: all sections render on one page; clicking a rail icon scrolls to / activates its
    section; scrolling updates the active icon; the theme toggle flips the root theme and writes
    `uiTheme`; the staged Apply still writes dirty keys (extend the MVP test).
  - Keybind capture: dispatching a `keydown` writes `zoomInVk`/`zoomOutVk`; a side-button
    `mousedown` writes `zoomInButton`/`zoomOutButton`.
  - Onboarding (`?mode=onboard`): the 3 steps render and advance; Next on the keys step fires the
    key `setConfig`s (apply-on-advance); reaching "Open Settings" fires `setConfig('onboarded','1')`
    and switches to the settings view.
- **Unit (doctest)**: add a `test_config` case for `onboarded` (default 0, parses). No other new
  pure logic (`ini_edit` already covers surgical writes).
- **Manual**: first-launch onboarding spawn; custom title bar drag / minimize / close; scroll-spy
  feel; the welcome animation; dark and light.

## File structure (new / changed)

- `ui/src/`: `App.svelte` (router), `Settings.svelte`, `Onboarding.svelte`, `lib/Rail.svelte`,
  `lib/Section.svelte`, `lib/KeybindCapture.svelte`, a scroll-spy action, `lib/Row.svelte`
  (extended), `theme` tokens, updated `settings-schema.js`, `bridge.js` (window/mode/openIni).
- `ui/tests/`: extended Playwright specs (settings scroll-spy + theme + keybind, onboarding flow).
- `src/config_ui/main.cpp`: frameless window, command-line mode, window/openIni bridge messages.
- `src/config.{h,cpp}`: `onboarded` key. `src/main.cpp`: first-launch spawn.
- `tests/test_config.cpp`: `onboarded` case.
- `mockups/`: the design mockups (committed as the visual reference).

## Out of scope (this phase)

- Actual login / licensing / user database (the account avatar is a placeholder; architecture
  leaves room).
- A loupe / non-fullscreen magnification mode (the welcome copy intentionally avoids saying
  "fullscreen" so it can be added later).
- A settings search box.
- Resizing the window between onboarding and settings (optional nicety, not required).
- A WebView2 bootstrapper for old Windows 10 (rely on Evergreen).

## Open decisions (resolved)

- Theme default is `auto` (follow OS), with a persisted manual override via the sun/moon toggle.
- Onboarding is 3 steps and configures only the zoom keys; speed and max live in Settings.
- Onboarding completion uses the generic `setConfig('onboarded','1')`, not a dedicated message.
