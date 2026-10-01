# Settings redesign (issue #303), 2026-10-01

Rebuild the Settings window (WindConfig.exe) to the design Max chose after nine mockup rounds, and
change how settings are kept: changes apply instantly but only for the session until saved.

## The target (match it, do not approximate it)

- `docs/design/settings-2026-10/FINAL-reference.png`: Max's screenshot of the final design. The built
  UI is reviewed against it side by side at the same window size, element by element (sizes, spacing,
  colours, fonts, icons, radii). "A similar look" is a failed review.
- `docs/design/settings-2026-10/FINAL-v10-grey.html`: the mockup source, the authority for exact
  tokens (colours, spacing, radii, font sizes, icon paths, the banner and slider CSS). Also
  `FINAL-v10-grey-dark.png` / `-light.png` and the banner image `c-grey.jpg`.
- Design history: `C:\Users\Admin\Documents\Claude\wind-settings-mockups\` (rounds 1-9, `keepers/README.md`).

### Visual spec (from the mockup)
- Window 1120x760 default, frameless; dark = pure #000, light = white. Segoe UI Variable for page text,
  Cascadia Mono for sidebar labels (weight 500), search placeholder, keycaps and the version.
- Title bar: white rounded-square wind logo, "Wind Settings", then right-aligned theme button
  (one button cycling Dark -> Light -> Auto, round moon / sun / half-circle glyphs), minimize, close.
  No profile selector in the title bar.
- Sidebar (240px): search field ("Search settings", Ctrl F hint), then the groups as rows with terminal
  line icons (square ends; the zoom and search lenses are round): Zoom, Moving around, Cursor, Follow
  typing, Colour, General. A hairline, an "EXPERT" label, then Advanced and About (with the version on
  the right) pinned to the bottom. Active row = soft grey fill, 8px radius, no outline. No counts, no
  sub-tabs, no dropdown arrows.
- Page banner per group: near-black band, the group icon in an outlined rounded box, large title,
  one-line description; the grey "smoke" aurora image fades in from the middle at very low opacity
  (start 50%, opacity 0.275). Light theme inverts the image.
- Content: sections as cards (10px radius, faint borders, card fill slightly above the page) with a
  small sentence-case caption above each ("Keys", "Levels", "Speed"). Rows: label + one-line
  description left, controls right-aligned to the card edge.
- Controls: monospace keycaps; "or" / "+" separators; "Add key" as a soft-fill chip (never dashed);
  sliders with a teal fill (#2fbfa5 dark / #11977f light), dark track and a bright end-cap knob
  (3x9px, no shadow), value right-aligned in tabular numbers; toggles and selects restyled to match.
- Unsaved changes: a floating capsule centred at the bottom ("N unsaved changes", Discard, Save),
  shown only when the session differs from the saved profile. No dots or "Changed" tags on rows.
- Search: typing filters every group (Advanced included) into a results page grouped by section;
  Esc or clearing returns to the group.

## Groups (task-based; every current setting keeps its ini key)

| Group | Cards and rows |
|---|---|
| Zoom | Keys: Zoom in, Zoom out, Zoom with the scroll wheel. Levels: Max zoom. Speed: Zoom-in speed, Zoom-out speed |
| Moving around | Keys: Pan left/right/up/down. Panning: Pan speed, Keep the mouse pointer, Mouse edge margin |
| Cursor | Look: High resolution cursor. Keys: Hide cursor, Inspect mode |
| Follow typing | Follow the text cursor, Follow keyboard focus, Keep the text cursor and focus |
| Colour | Warmth, Brightness |
| General | Appearance: Theme. Profiles: active profile + switch/create/rename/duplicate/delete (moved here from the old title bar). Files: Export diagnostics, Open settings file |
| Advanced | Engine: Magnifier engine (+ per-window engines when Auto). Apps: Never use Render for, Pass zoom keys to these apps, Mouse-locked games. Fine tuning: Cursor speed, Pan smoothing, Zoom-in ease, Ease-in duration, Release glide, Frametime logging |
| About | Logo, version, links (today's About content) |

"Show advanced settings" goes away (Advanced is now always a group); the `showAdvanced` key stays
readable and is ignored.

## Session model (instant apply, explicit save)

Today every setting is staged until Apply, and the active profile is live-bound (each write is mirrored
into `profiles\<name>.ini`). New model:

- **Live ini = this session.** Every change writes `magnifier.ini` at once (the core hot-reloads it as
  today), so it takes effect immediately. It is NOT mirrored into the profile file.
- **Profile file = saved.** "Save" writes the live profile-scoped snapshot into the active profile's
  file (`MakeProfileText`). "Discard" rewrites the live ini from the profile (`MakeLiveText`).
- **Unsaved = live differs from the profile** in profile-scoped keys (pure
  `SessionDiffers(liveText, profileText)` in `src/profiles.*`, unit-tested). The UI shows the capsule
  from the same comparison, which the host reports with the config (`saved` alongside `values`).
- **Reset when Wind closes:** at Wind start, the core rewrites the live ini from the active profile
  (`MakeLiveText`) before parsing it, so unsaved changes never survive a restart or a crash. Exception:
  a restart Wind triggers itself (engine change, profile switch with a model change) first writes
  `%LOCALAPPDATA%\Wind\session.keep`; the starting core sees it, skips the reset once and deletes it.
- **Kept as they are:** keybind captures save immediately (live AND profile, single key via
  `UpdateIniText`), because a half-saved bind is confusing and binds are deliberate. Global keys
  (`profile`, `onboarded`, `uiTheme`, `showAdvanced`) are written directly as today. The MPO /
  High resolution cursor row keeps its confirm step (UAC + restart prompt) as an inline action.
  The engine row applies on an inline "Restart Wind" button rather than on selection.
- **Prompts:**
  - Closing Settings with unsaved changes: "Save", "Discard", "Keep for this session" (closes the
    window, changes stay live until Wind quits), Cancel via Esc.
  - Quitting Wind (tray Quit, or Quit from Settings) with unsaved changes: Save / Discard / Cancel.
    The tray checks `SessionDiffers` itself (it already reads the ini and profiles) and shows a
    TaskDialog; Save mirrors live into the profile, then quits.
  - Switching profile with unsaved changes: Save / Discard / Cancel before switching.
  - Windows shutdown/logoff: no prompt; the next start resets the session (documented).
- Crash recovery of the WebView (draft hand-back) becomes unnecessary for settings (they are already
  live) and is removed; the host's engine-recreate path stays.

## Theme
`uiTheme` (auto/dark/light) drives the Settings window as today, and now also the tray: WindTray reads
`uiTheme` from the ini on every menu open (auto = system setting) instead of always following the
system. (The tray menu restyle itself is a separate follow-up, from its own mockups.)

## Framework and start-up
- Upgrade to Svelte 5 + the matching vite plugin. New components are written with runes; Svelte 5's
  legacy mode keeps any untouched component compiling.
- Faster start: the host injects the initial config (values + saved + profiles) with
  `AddScriptToExecuteOnDocumentCreated` so the first render needs no round trip; the window background
  is set to the theme colour before WebView2 paints (no white flash); the onboarding bundle and the
  aurora image load lazily. Start-up is measured (host launch to first meaningful paint, logged to
  `wind-config.log`) before and after; the PR states both numbers.

## Out of scope
New settings or features, the tray menu restyle (follow-up after its mockups), onboarding flow
changes (it gets the new tokens, same steps), resizable-window layouts beyond the current min size.

## Testing
- doctest: `SessionDiffers` (global keys ignored, missing keys = defaults, ordering, whitespace),
  session reset + `session.keep` logic (pure part), single-key profile update.
- Playwright (ui/tests, mock bridge): groups and navigation, search (finds Advanced rows, Esc
  returns), instant apply posts `setConfig` without a Save, capsule appears/disappears, Save and
  Discard messages, close prompt with three choices, profile switch prompt, keybind saves immediately,
  theme cycle, light theme, keyboard focus order, axe-style a11y checks (existing a11y.spec.js updated).
- Visual: a Playwright screenshot of the built UI at 1120x760 compared with
  `FINAL-reference.png`; a reviewer agent lists every visible difference and each is fixed or
  justified in the PR.
- Manual on the deployed build: open Settings, change a slider (takes effect at once), close with
  "Keep for this session", quit from the tray (prompt), restart Wind (session reset), engine change
  restart keeps the session.
