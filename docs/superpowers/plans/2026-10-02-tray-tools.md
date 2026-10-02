# Tray tools Implementation Plan

**Goal:** Engine dropdown, App fixes manager and Pause Wind in the tray flyout, per
`docs/superpowers/specs/2026-10-02-tray-tools-design.md`.

**Branch:** `feat/315-tray-tools` (worktree `Wind-tray2`), stacked on `feat/313-tray-flyout`; PR targets
main after #312 and #314. Version bump 0.18.0. No em-dashes. Pure logic without `<windows.h>`.
Gates: `build.bat test`, `build.bat`, `build.bat tray`, `build.bat config`, `cd ui && npx playwright test`.

## Review Focus
1. The foreground category must ignore the taskbar, the tray flyout and Wind's own windows, or the dropdown always says "Other".
2. Listening must ignore the taskbar, tray, alt-tab switcher, desktop and Wind windows, and a minimized fullscreen game must count when you return to it.
3. Pause must release any swallowed keys and zoom out cleanly; unpausing restores swallowing.
4. App-fix writes must hit both the live ini and the profile, and survive a Wind restart (session reset must not drop them).
5. Shared-block version bump: an older WindTray with a newer Wind (and the reverse) must not crash.
6. The dropdown popup and the pulsing chips follow the flyout's placement, theme, keyboard-only focus ring, no tooltips.

### Task 1: Core publishing (category, recent apps, pause)
Files: `src/tray_ipc.h` (fgCategory, fgExe of the last real foreground app, paused, command seq; version 2), `src/main.cpp`
(publish on foreground change, filtered; wait on `Local\Wind_TrayCommand`; pause gating), pure helpers
in `src/tray_publish.h` with doctest.

### Task 2: Tray item model
`src/tray_items.*`: new items `engine` (dropdown), `fixLock`, `fixPass`, `pause`; defaults off; tests.

### Task 3: Flyout controls
`src/tray_app/flyout_*`: engine dropdown chip + list popup (writes the per-kind engine key, session),
Mouse lock and Pass keys listen chips (pulse on chip and tray icon; target = next real foreground app via fgExe; toggle the exe in lockApps / noSwallowApps in live + profile; rising/falling chime WAV resources; cancel by re-click; 20 s timeout), Pause chip; disabled
state when `model` is not Auto; icons in the terminal line style; render-test cases.

### Task 4: Settings Tray menu tab
`ui/src/tray/*`: four new list items (Engine for the app in front, Mouse lock (listen), Pass keys (listen), Pause Wind) with icons (no background) and descriptions; Playwright tests.

### Task 5: Docs, version, PR
Spec table, architecture tray chapter, CLAUDE.md tray notes; `src/version.h` 0.18.0; gates; push; PR
closing #315 that states it stacks on #314. Then deploy for Max to test.
