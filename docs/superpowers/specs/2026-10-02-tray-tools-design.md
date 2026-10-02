# Tray tools: engine dropdown, app fixes, pause (issue #315), 2026-10-02

Follow-up to the tray flyout (#313, PR #314). Three new things in the flyout's control row, chosen by
Max from a list of suggestions. Stacks on PR #314.

## 1. Engine dropdown (in the chip row, next to the toggle chips)
- A chip-sized dropdown showing the engine used for the KIND of window that was in front before the
  flyout opened: Game, Blurred window, Desktop or Other window (Wind's existing categories,
  `src/engine_pick.h`). Values: Auto, Transform, Render, the same as the Settings > Advanced
  per-window engine rows (`engineGame`, `engineAcrylic`, `engineDesktop`, `engineOther`).
- Picking a value writes that key to the live ini only. The core already reads these per pick, so it
  applies at once with no restart (hot), including mid-zoom (hybrid re-picks on change). It is a
  SESSION change, exactly like picking it in Settings: nothing is saved, opening Settings shows the
  unsaved capsule, closing Settings gives the Save / Discard / Keep prompt, the Quit prompt counts it.
- The chip shows a small engine glyph plus a short mono label ("Auto" / "Tx" / "Rd" style is too
  cryptic, so the label is the full word in the open list and an icon on the chip); the open list
  has a caption naming the category ("Engine for games").
- Which category: the core publishes the category of the last real foreground window (ignoring the
  shell, the taskbar, the tray flyout and Wind's own windows) in `TrayShared` (new field
  `fgCategory`, version bump of the shared block). The flyout reads it when it opens.
- When the main engine (`model`) is not Auto, the per-window rows do nothing, so the dropdown is
  shown disabled with "Main engine: Render" (switching the main engine needs a restart and stays in
  Settings).

## 2. App fixes: listen-and-chime chips (Max, 2026-10-02)
Two chips in the toggle row, one per fix:
- **Mouse lock** (`lockApps`: the view follows hand movement in games that hold the pointer).
- **Pass keys** (`noSwallowApps`: zoom keys also reach the app, fixes stuttery panning).

How it works:
1. Click the chip: it starts **listening**. The chip pulses (a slow, soft brightness pulse, ~1.2 s
   period), and the tray icon pulses too, because the flyout closes as soon as you click elsewhere.
2. Go to the app however you like: click its window, alt-tab to it, or click its taskbar button.
   The next real foreground app is the target (the taskbar, the tray, Wind's own windows, the
   alt-tab switcher and the desktop are skipped; a fullscreen game that minimized counts when you
   come back to it).
3. Wind toggles that fix for that app: if the app did not have it, it is added and a short
   **rising chime** plays (about half a second, two soft notes going up); if it already had it, it
   is removed and the **falling chime** (the same notes going down) plays. Listening ends.
4. Clicking the chip again while it pulses cancels (no sound). Listening also ends silently after
   20 seconds.
- The change PERSISTS immediately (live ini AND the active profile), like keybinds: these are
  deliberate per-app fixes you expect next time.
- The chimes are two tiny WAV resources in WindTray, played asynchronously, at a modest level.
- The chips themselves carry no on/off state (the state is per app); Settings > Advanced lists the
  apps for anyone who wants to review them.
- Recency is no longer needed, so the core publishes no app ring; it only needs the foreground
  filtering it already does for the engine dropdown's category.

## 3. Pause Wind (toggle chip)
- ON = zoom keys and the scroll-wheel zoom do nothing and are not swallowed (they reach apps), the
  magnifier stays loaded; OFF = normal. Runtime only: not saved, resets when Wind restarts.
- The tray sets `TrayShared::paused` and signals a new `Local\Wind_TrayCommand` event that the core
  adds to its wait set (the 1x loop sleeps, #71). The core zooms out if paused while zoomed. The tray
  icon gets a small paused mark so the state is visible when the flyout is closed.

## Settings: Tray menu tab
The Toggles list gains "Pause Wind", "Mouse lock (listen)" and "Pass keys (listen)"; the engine dropdown is a list item too
("Engine for the app in front"). All three default OFF (not shown) except nothing else changes.
Limits unchanged (4 sliders; toggles uncapped; the dropdown counts as a toggle-row item).

## Decisions (Max, 2026-10-02)
1. Engine dropdown = per-window-kind engine, hot, a session change exactly like Settings.
2. App fixes = two listen-and-chime chips (Mouse lock, Pass keys); changes persist immediately.

## Testing
doctest: category publishing filter (shell/tray/Wind windows ignored), listen state machine (target filter, toggle add/remove, cancel, 20 s timeout),
add/remove exe in the lists (case-insensitive, no duplicates), pause gating of zoom input. Playwright:
the three new rows in the Tray menu tab. Flyout render test with the dropdown and app-fixes list open.
Manual: game in front -> flyout -> engine dropdown says "games" and switches live; Mouse lock chip pulses, alt-tab into the game plays the rising chime,
again plays the falling chime, the change survives a Wind restart; Pause stops zoom keys and they type
normally.
