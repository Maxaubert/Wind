# Settings redesign Implementation Plan

> **For agentic workers:** execute task by task in order (later tasks consume earlier interfaces).
> Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Rebuild WindConfig's Settings UI to match `docs/design/settings-2026-10/FINAL-reference.png`
exactly, and switch settings to instant-apply with an explicit Save (session layer), per the spec.

**Architecture:** The live `magnifier.ini` becomes the session; the active profile file becomes the
saved state. Pure diff/merge helpers in `src/profiles.*`; the core resets the session at start; the
host gains save/discard/persist messages; the Svelte app (upgraded to Svelte 5) is rebuilt as a shell
(title bar, sidebar, banner, cards, capsule) over a regrouped schema.

**Tech Stack:** C++17 / MSVC, doctest, WebView2, Svelte 5 + Vite, Playwright.

**Spec:** `docs/superpowers/specs/2026-10-01-settings-redesign-design.md`

## Global Constraints
- No em-dash characters anywhere (code, comments, docs, UI copy, commits).
- Every ini key keeps its name and meaning; no setting is added or removed (except the UI-only
  "Show advanced settings" row, whose key stays parsed and ignored).
- Pure-logic files must not include `<windows.h>`.
- Exact visual tokens come from `docs/design/settings-2026-10/FINAL-v10-grey.html`; do not invent values.
- Branch `feat/303-settings-redesign` in the worktree `Wind-settings`; one PR; version bump to 0.16.0 inside it.
- Gates before the PR: `build.bat test` (doctest exit 0), `build.bat config`, `cd ui && npx playwright test`.

## Review Focus
1. A restart Wind triggers itself (engine change, profile switch with model change) must keep the session; a plain restart must reset it.
2. Keybind captures persist immediately even while other changes are unsaved, and Save afterwards does not lose them.
3. Tray Quit with unsaved changes while Settings is closed still prompts (the tray decides from the files, not from the UI).
4. Light theme: the aurora image, capsule, cards and sidebar highlight all read correctly on white.
5. Search reaches Advanced rows and keyboard-only use (Tab, Esc, Ctrl+F) works with visible focus.

---

### Task 1: Session helpers (pure)

**Files:** Modify `src/profiles.h`, `src/profiles.cpp`; Test `tests/test_profiles.cpp` (or the existing profiles test file).

**Interfaces (produces):**
- `bool SessionDiffers(const std::string& liveText, const std::string& profileText);`
  true when any profile-scoped key differs (`IsGlobalProfileKey` keys ignored; a key missing on one
  side compares as missing, not as a default; values compared trimmed).
- `std::string UpdateProfileKey(const std::string& profileText, const std::string& key, const std::string& value);`
  thin wrapper over `UpdateIniText`, refusing global keys (returns input unchanged).

- [ ] Write failing doctest cases: identical texts -> false; one changed key -> true; only `uiTheme`
      differs -> false; key present only in live -> true; CRLF vs LF and trailing spaces -> false;
      `UpdateProfileKey(..., "uiTheme", ...)` leaves text unchanged.
- [ ] `build.bat test` -> the new cases fail.
- [ ] Implement with `ReadIniValues` + `IsGlobalProfileKey`.
- [ ] `build.bat test` -> pass. Commit `feat(profiles): session diff helpers (#303)`.

### Task 2: Core resets the session at start

**Files:** Modify `src/main.cpp` (startup, before the first `ParseConfig`), `src/profiles_io.h` (I/O helper).

**Interfaces:** `void ResetSessionToProfile(const std::wstring& iniPath);` in `profiles_io.h`:
if `%LOCALAPPDATA%\Wind\session.keep` exists, delete it and return; else, when the active profile file
exists, write `MakeLiveText(profileText, liveText)` to the ini atomically (only if `SessionDiffers`).
`std::wstring SessionKeepPath();` shared with the host.

- [ ] Call it right after `EnsureProfilesSeeded(iniPath)` in `src/main.cpp`.
- [ ] Log one Info line when a session was reset or kept.
- [ ] Build `build.bat` and `build.bat test`. Commit `feat(core): unsaved settings reset when Wind starts (#303)`.

### Task 3: Host bridge for the session model

**Files:** Modify `src/config_ui/main.cpp`; `ui/src/bridge.js`.

**Interfaces (host messages):**
- `getConfig` reply adds `"saved":{...}` (the active profile's values merged over live globals) and `"profiles":{names,active}`.
- `setConfig {key,value}`: writes the live ini only (the live-bound mirror is removed).
- `setConfigPersist {key,value}`: writes live AND the active profile (`UpdateProfileKey`); used by keybinds.
- `saveSession`: writes `MakeProfileText(live)` to the active profile; replies `{type:'sessionSaved',ok}`.
- `discardSession`: writes `MakeLiveText(profile, live)` to the ini; replies `{type:'config',...}` (fresh).
- `window` `restartWind` (and model-changing profile switches) write `SessionKeepPath()` first.
- `window` `quitWind`: unchanged message; the UI prompts first.
- Initial config injection: before `Navigate`, `AddScriptToExecuteOnDocumentCreated` sets
  `window.__windInit = {values, saved, profiles, theme}`; `getConfig()` in bridge.js resolves from it on first call.
- Window background set from `uiTheme` (and system theme for auto) via `ICoreWebView2Controller2::put_DefaultBackgroundColor` and the class brush, so no white flash.
- Remove the `draft` / `restoreDraft` path (settings are live now); keep engine recreate.

- [ ] Implement; update `bridge.js` (`saveSession()`, `discardSession()`, `setConfigPersist(k,v)`, init fast path).
- [ ] `build.bat config` builds. Commit `feat(config): session save/discard bridge (#303)`.

### Task 4: Tray follows the app theme and guards Quit

**Files:** Modify `src/tray_app/tray_draw.h` (`MakePalette(bool dark)`), `src/tray_app/tray_menu.cpp`.

- [ ] Resolve dark/light from the ini's `uiTheme` on each open (`auto` -> `SystemUsesLightTheme()`).
- [ ] Before `RequestWindQuit()`: read live + active profile, `SessionDiffers` -> TaskDialog "You have
      unsaved settings" with Save / Discard / Cancel. Save writes `MakeProfileText(live)` to the profile.
- [ ] Build `build.bat tray`. Commit `feat(tray): theme from Wind, unsaved prompt on Quit (#303)`.

### Task 5: Svelte 5 upgrade

**Files:** `ui/package.json`, `ui/vite.config.*`, existing components only as needed to compile.

- [ ] Bump `svelte` to ^5, `@sveltejs/vite-plugin-svelte` to the Svelte 5 major, `vite` as required.
- [ ] `npm install`, `npm run build`, `npx playwright test` green on the OLD UI before any redesign
      (fix only what the upgrade breaks). Commit `chore(ui): Svelte 5 (#303)`.

### Task 6: Design tokens, shell and icons

**Files:** Create `ui/src/design/tokens.css`, `ui/src/design/icons.js` (terminal icon set from the
mockup: zoom, move, cursor, typing, colour, general, adv, about, search, auto/light/dark), 
`ui/src/shell/TitleBar.svelte`, `ui/src/shell/Sidebar.svelte`, `ui/src/shell/Banner.svelte`,
`ui/src/shell/Card.svelte`, `ui/src/shell/SaveCapsule.svelte`; copy `c-grey.jpg` to `ui/public/`.

- [ ] Port every token and measurement from `FINAL-v10-grey.html` (dark + light), including the
      v10 overrides (fainter lines, soft grey fill highlight, 8px items, 10px cards).
- [ ] Each shell component renders in isolation with props only (no bridge calls).
- [ ] Commit `feat(ui): design tokens and shell components (#303)`.

### Task 7: Regrouped schema and pages

**Files:** Rewrite `ui/src/settings-schema.js` to the spec's group table (ids: zoom, move, cursor,
typing, colour, general, advanced, about; each group: label, icon, desc, cards[{caption, rows}]);
restyle `ui/src/lib/Row.svelte` controls into `ui/src/controls/` (Keycaps, Slider with end-cap,
Toggle, Select, AppList, HighRes, EngineRow with inline "Restart Wind").

- [ ] Every row of the old schema appears exactly once (a test enumerates old keys vs new).
- [ ] Commit `feat(ui): task-based groups and restyled controls (#303)`.

### Task 8: Session flow in the UI

**Files:** Rewrite `ui/src/Settings.svelte` (state, prompts), `ui/src/lib/ProfileMenu.svelte` ->
`ui/src/general/Profiles.svelte` (General page).

- [ ] Changes call `setConfig` at once; keybinds call `setConfigPersist`; dirty = values vs saved.
- [ ] Capsule Save -> `saveSession()`; Discard -> `discardSession()` then reload values.
- [ ] Close prompt (Save / Discard / Keep for this session); Quit Wind prompt (Save / Discard / Cancel);
      profile switch prompt (Save / Discard / Cancel); MPO and engine confirm flows preserved.
- [ ] Commit `feat(ui): instant apply with Save, session prompts (#303)`.

### Task 9: Search

**Files:** Create `ui/src/search/search.js` (pure: query -> matching rows across groups by label,
description, card caption and group label; case-insensitive, word-prefix) and `ui/src/search/Results.svelte`.

- [ ] Ctrl+F focuses search; typing shows results grouped by group; Enter jumps to the first;
      Esc clears and returns. Commit `feat(ui): settings search (#303)`.

### Task 10: Onboarding tokens
- [ ] Onboarding uses the new tokens/buttons; steps unchanged. Commit `style(ui): onboarding on new tokens (#303)`.

### Task 11: Start-up measurement
- [ ] Host logs launch-to-first-paint (NavigationCompleted + first `ready` message from the page);
      record the old build's number and the new one in the PR. Lazy-load onboarding and the banner image.
      Commit `perf(config): measured faster first paint (#303)`.

### Task 12: Tests
- [ ] Update `ui/tests/settings.spec.js`, `a11y.spec.js`, `onboarding.spec.js` to the new UI and add the
      spec's Playwright cases (groups, search, instant apply, capsule, Save/Discard, three prompts,
      keybind persist, theme cycle, light theme, focus order).
- [ ] Commit `test(ui): redesign coverage (#303)`.

### Task 13: Match the reference
- [ ] Playwright screenshot of the built UI (dark, Zoom page, two unsaved changes) at 1120x760 next to
      `FINAL-reference.png`; a reviewer lists every visible difference; fix until none remain or each is
      justified (for example real data differs). Light theme against `FINAL-v10-grey-light.png`.
- [ ] Commit `style(ui): pixel match to the reference (#303)`.

### Task 14: Docs, version, PR, deploy
- [ ] Update `docs/architecture/` (settings UI + session model), the CLAUDE.md lines about the
      staged Apply/Discard footer and live-bound profiles, bump `src/version.h` to 0.16.0.
- [ ] Gates green; push; open the PR (issue #303) with before/after start-up numbers and screenshots.
- [ ] Deploy to `C:\Program Files\Wind` via `tools\uiaccess_setup.ps1`, run the spec's manual checks,
      then ask Max "merge?".
