# Tray flyout Implementation Plan

> **For agentic workers:** execute task by task in order. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Replace WindTray's owner-drawn menu with the j01 flyout (quick controls) and add the
Settings "Tray menu" tab that chooses and orders them.

**Architecture:** Pure item-list logic in `src/tray_items.*` (parse/serialise/defaults, shared by
WindTray and the config host). WindTray gains a Direct2D flyout window (`src/tray_app/flyout_*`) that
reads `TrayShared` + the ini and writes the live ini. The core gains a tray command event for Hide
cursor. The Settings UI gains a group with two drag-reorder lists.

**Tech Stack:** C++17 / MSVC, Direct2D, DirectWrite, WIC, doctest, Svelte 5, Playwright.

**Spec:** `docs/superpowers/specs/2026-10-01-tray-flyout-design.md`

## Global Constraints
- Branch `feat/313-tray-flyout` (worktree `Wind-tray`), stacked on `feat/303-settings-redesign`;
  the PR targets main and is merged only after #312.
- No em-dashes. Pure files without `<windows.h>`. WindTray stays non-UIAccess.
- Visual tokens come from `docs/design/tray-2026-10/j01-calm-tint.html` and
  `docs/design/settings-2026-10/FINAL-v10-grey.html`; nothing invented.
- Version bump to 0.17.0 in the PR. Gates: `build.bat test`, `build.bat`, `build.bat tray`,
  `build.bat config`, `cd ui && npx playwright test`.

## Review Focus
1. "Keep within the edges" writes BOTH mouseAlign and trackAlign, and reads ON only when both are 1 (a hand-edited mixed state shows OFF; clicking sets both).
2. Dragging a slider must not flood the ini (throttle) and the final value must land on release.
3. Flyout placement with the taskbar on the left/top/right and on a secondary monitor, at 100-250% DPI.
4. Dismissal: clicking outside, Esc, alt-tab and the tray icon itself all close it exactly once (no reopen flicker).
5. Disabled items and unknown keys in `traySliders`/`trayToggles` from an older or hand-edited ini.

---

### Task 1: Tray item model (pure)
**Files:** Create `src/tray_items.h/.cpp`; Test `tests/test_tray_items.cpp`; add the five keys to `IsGlobalProfileKey`.
**Interfaces:** `struct TrayItem { std::string key; bool on; };`
`struct TrayLayout { bool perf; std::vector<TrayItem> sliders, toggles; };`
`TrayLayout ParseTrayLayout(const IniValues&);` `void WriteTrayLayout(const TrayLayout&, IniValues&);`
`const std::vector<std::string>& EligibleSliders(); const std::vector<std::string>& EligibleToggles();`
- [ ] Failing tests: defaults (perf off, warmth+brightness on), round trip, order kept, unknown keys dropped, missing eligible items appended off, cap (more than 4 enabled sliders are read as off, in list order; toggles uncapped). Sliders: colorWarmPct, colorDimPct, maxLevel, zoomInSpeed, zoomOutSpeed, panSpeed, cursorSmoothing, zoomEaseOutMs. Toggles: trackCaret, trackFocus, keepEdges (the combined mouseAlign + trackAlign item).
- [ ] Implement; `build.bat test` green; commit `feat(tray): tray layout model (#313)`.

### Task 2: (removed)
No action buttons were chosen, so the core needs no tray command. Skip.

### Task 3: Flyout rendering
**Files:** Create `src/tray_app/flyout_window.cpp/.h` (window, placement, dismissal), `src/tray_app/flyout_draw.cpp/.h` (D2D/DWrite drawing from a view model), embed `c-grey.jpg` in `wind_tray.rc`; remove the HMENU path from `tray_menu.cpp` (keep icon/IPC parts).
- [ ] Placement maths as a pure function with doctest cases (four taskbar edges, multi-monitor, DPI).
- [ ] `--render-test <png>` renders the flyout with fake status to a file.
- [ ] Commit `feat(tray): flyout window and drawing (#313)`.

### Task 4: Flyout interaction
- [ ] Hit-testing, slider drag (throttled ini writes, final on release), chips, tooltips, keyboard, profile list popup, Settings and Quit (with the #303 Quit prompt), live performance refresh at ~10 Hz only while open.
- [ ] doctest for the throttle and value formatting; commit `feat(tray): quick controls (#313)`.

### Task 5: Settings "Tray menu" tab
**Files:** `ui/src/settings-schema.js` (group under a TRAY label), `ui/src/tray/TrayMenuPage.svelte`, `ui/src/tray/DragList.svelte` (reusable smooth drag list with keyboard reorder), icons added to `ui/src/design/icons.js`.
- [ ] Performance toggle card; Sliders and Toggles cards; icons without background; checkmarks; count "N of 4" on Sliders (Toggles show "N on"); at the slider cap unchecked checkmarks are disabled with "Uncheck one to add another"; writes the five keys.
- [ ] Commit `feat(ui): Tray menu tab (#313)`.

### Task 6: Tests
- [ ] Playwright: tab render, check/uncheck, drag in both lists, keyboard reorder, keys written. Commit `test: tray flyout and tab (#313)`.

### Task 7: Match the references
- [ ] `WindTray.exe --render-test` vs `j01-calm-tint-dark.png`/`-light.png`; Settings tab screenshot vs `k01` (icons without background). List every difference, fix until none or justified. Commit `style: match the tray references (#313)`.

### Task 8: Docs, version, PR
- [ ] Update `docs/architecture` (tray chapter), the CLAUDE.md "Three binaries" tray notes and `docs/superpowers/specs/2026-08-28-tray-menu-design.md` (mark superseded). Bump `src/version.h` to 0.17.0.
- [ ] Gates green; push; PR closing #313 with base main, noting it stacks on #312. Do not merge.
