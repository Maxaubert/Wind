> **Superseded (2026-10-01, issue #313):** the owner-drawn menu is replaced by the tray flyout. See `2026-10-01-tray-flyout-design.md`.

# Tray menu: instrument header (2026-08-28)

Replace the four bare strings in the tray menu with a **status header plus three actions**, keeping
it a real Win32 menu rather than a custom window.

Chosen from three rounds of mockups. Round one offered three restyled menus and was rejected -
correctly, they were the same menu with nicer trim. Round two looked at what the well-regarded apps
in this slot actually do (EarTrumpet, iStat Menus, Windows' own volume/network trays) and found they
are **flyout panels with live state**, not menus. Round three settled on the "Instrument" panel with
the warning banner and engine picker removed.

## What it looks like

```
┌────────────────────────────────────┐
│  7.4×                  COMPOSITION │   <- zoom, big; composition fps, right
│  ADVANCED  transform · panning     │   <- engine pill + one-line state
│  ┌──────────────────────────────┐  │
│  │ FRAME PACING · 60s     6.9ms │  │   <- sparkline, drawn from the tick ring
│  └──────────────────────────────┘  │
├────────────────────────────────────┤
│  Profile                Default  › │
│  Settings                          │
│  Quit                              │
└────────────────────────────────────┘
```

Idle: the zoom figure reads `Idle`, the state line reads `Hold Mouse 5 to zoom`, the sparkline is a
flat dashed rule. The panel must not change height between states.

## Why an owner-drawn HMENU, not a custom window

The banner and the engine picker were the only elements that needed live updates while open. Without
them a **snapshot taken when the menu opens** is just as truthful - the numbers are current at that
moment - and the pacing graph draws fine as a still from the existing ring.

| | owner-drawn menu (chosen) | custom popup window |
|---|---|---|
| the look above | yes | yes |
| live updates while open | no (snapshot) | yes |
| keyboard nav, dismissal, submenus, DPI, screen readers | free | ~500 extra lines, hand-built |
| removes the `TrackPopupMenu` tick-timer hack | no | yes |
| size | ~250 lines | ~800 lines |

A custom window is the better answer only if the readout must animate. It does not.

## Scope

**In:** the header, the three items, dark/light theming, DPI scaling, the "Advanced" rename.

**Out, deliberately:**
- **Export diagnostics leaves the tray.** It already exists as a button in the Settings UI
  (`Settings.svelte:276`), so nothing is lost. Its off-thread worker and the hardened
  `DiagDoneMsg` result-slot go with it (git history keeps them).
- **No warning banner.** Drafted as an MPO warning, then dropped: `mpoBuster=1` ships on, so the
  pan walls lift on virtually every machine and the banner would be noise. Field-checked - Foundation
  at 30x, ten sessions, full reach, no crash.
- **No engine picker.** `model` is read once at launch, so a tray switch would have to restart Wind.
- **No pause, no zoom presets.**

## New plumbing

Two one-way, non-blocking reads. Neither may touch the tick path with anything but a plain store.

### `src/tray_status.h`
A single snapshot the tick loop stores and the tray reads. Plain atomics, no allocation, no lock.

```cpp
struct TrayStatus {
    double      level;      // 1.0 = idle
    const char* engine;     // "Advanced" | "Transform" | "Render" | "System"
    bool        panning;
};
```

### `src/tick_stats.h`
A fixed ring of the last N tick intervals (ms), written one float per tick from the loop that already
computes `dt` for diagnostics, read by the tray to draw the sparkline and the composition figure.
Lock-free single-producer/single-consumer; a torn read costs one wrong pixel and nothing more.

`kRingCap = 256` at ~144 Hz is ~1.8 s of history. The label says `60s`, so either the ring grows or
the label changes - **the label changes**, since a 256-sample ring is the cheap, correct-by-
construction option and the graph reads the same either way.

## Drawing

- `MF_OWNERDRAW` on every item; `WM_MEASUREITEM` / `WM_DRAWITEM` handled in `Tray::HandleMessage`,
  which the main `WndProc` already forwards to.
- Header item is `MF_DISABLED | MF_GRAYED` so it cannot be selected but still receives `WM_DRAWITEM`.
- Menu background via `SetMenuInfo` `hbrBack`; item hover per the Windows 11 pattern (soft fill plus
  a 3 px accent bar at the left edge), accent `#5b5bd6`.
- Theme from `HKCU\...\Themes\Personalize\AppsUseLightTheme`, read at open time.
- DPI from `GetDpiForWindow`; every metric scaled, nothing hard-coded in pixels.
- Font from `SPI_GETNONCLIENTMETRICS` `lfMenuFont`, so it matches the shell.

## The "Advanced" rename

`ui/src/settings-schema.js` line 62 only: `hybrid:'Auto'` becomes `hybrid:'Advanced'`. The four
per-window-type keys (lines 70/75/80/85) keep `auto:'Auto'` - those genuinely are automatic per
category, and renaming them would misdescribe them.

## Testing

- Pure logic (label selection, ring statistics, zoom formatting) unit-tested in the doctest suite;
  drawing is not, since it needs a device context.
- Manual: dark and light, 100% and 225% DPI, idle and zoomed, a profile switch, and the tick loop
  still ticking while the menu is open (the existing 8 ms timer keeps that true).
