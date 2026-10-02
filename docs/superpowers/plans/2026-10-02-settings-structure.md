# Settings structure, hotkeys and themes: implementation plan (#318)

Spec: `docs/superpowers/specs/2026-10-02-settings-structure-design.md` (+ the decisions log next to it).
Branch `feat/318-settings-structure` (worktree `Wind-settings2`), stacked on `feat/315-tray-tools`.
Version 0.19.0. No em-dashes. Gates: `build.bat test`, `build.bat`, `build.bat tray`, `build.bat config`,
`cd ui && npx playwright test` (run `cmd.exe /c ".\build.bat X"` from PowerShell, never from bash).
Reference mockups: `C:/Users/Admin/Documents/Claude/wind-settings-mockups/ia/ia07.html` (structure, copy,
hotkeys, profile dialogs), `ia08.html` + `palettes08.cjs`, `palettes-b1.cjs`, `palettes-b2.cjs` (themes).

## Review focus
1. Old inis: `zoomWheelMods` migrates once into free zoom slots (both full -> keep the wheel mods setting
   and log), unknown `uiPalette` reads grey, missing `panKeysOn`/`hideCursorOn`/`cursorLockOn` read 1.
2. A wheel binding never swallows plain scrolling (only with its modifiers), and a disabled extra key is
   neither bound nor swallowed.
3. Session model still holds: every new UI control is a session change except keybinds (persist at once,
   as today); `uiPalette` and `showAdvanced` are global and survive profile switches.
4. Advanced rows hidden when the switch is off but found by search; no section with fewer than 2 rows.
5. Every theme x mode (grey, ember, ocean, hicon; dark and light) in Settings and in the tray flyout render test.

## Tasks
1. **Core** (`src/config.*`, `src/input_router.*`, `src/main.cpp`, `src/profiles.cpp`): button codes 6/7 =
   wheel up/down in the zoom button slots (+ButtonMods), per-notch zoom on a matching wheel event, the
   migration from `zoomWheelMods`, the three `*On` switches gating bind + swallow, `uiPalette` as a global
   UI-only key. doctest for parse, migration, gating and the wheel match.
2. **Settings structure + copy** (`ui/src/settings-schema.js`, `ui/src/shell/*`, `ui/src/Settings.svelte`,
   search): tabs/order/divider, opens on Hotkeys, sections and copy from the spec, renderExclude row gone,
   title-bar theme button gone, `showAdvanced` reveals adv rows inline, search finds hidden adv rows.
3. **Hotkeys page** (`ui/src/controls/*`): one-box bindings with + inside, click to re-record, hover x,
   limits (2 for zoom, 1 otherwise, max 2 modifiers), wheel capture on zoom rows, pan box with Arrow keys
   and the unset state, Extra keys switches (dim the binding when off).
4. **Preferences** (`ui/src/general/*` or a new `ui/src/prefs/*`): Mode three-way, Profile dropdown with
   trash + confirm dialog and the New dialog, Troubleshooting section; theme tokens
   `ui/src/design/themes.css` generated from the mockup palettes (4 themes x 2 modes (grey, ember, ocean, hicon)) applied by
   `uiPalette` + mode; a plain temporary one-row theme picker (final layout comes from Max's mockup pick).
5. **Tray theming** (`src/tray_app/flyout_*`): `flyout_palettes.h` with the same 4 x 2 palettes, the
   flyout reads `uiPalette`/`uiTheme` on open, sharp radii for hicon; render-test flag `--palette <id>`.
6. **Docs, version, PR**: spec/plan final, CLAUDE.md notes, `src/version.h` 0.19.0, gates, push, PR that
   closes #318 and says it stacks on #316.

## Status (2026-10-02)
All six tasks are built on `feat/318-settings-structure` (version 0.19.0). The picker is mockup option A with four
themes; see "As built" in the spec. Not pushed and not merged: waiting for Max's review of the PR.
