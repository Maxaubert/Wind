# Profiles - design

Date: 2026-08-12
Status: approved

## Purpose

Named settings profiles for different activities (e.g. Gaming / Desktop / Reading). Switchable
from the tray menu and from the settings UI titlebar. Profiles are full snapshots: they carry
every setting including keybinds. The active profile is live-bound: applying changes in the
settings UI saves into it (no separate "save to profile" step).

## Decisions (from brainstorming)

- Use case: different activities, switched often.
- Keybinds are PER-PROFILE (full snapshots).
- Active profile is LIVE: Apply writes into the profile. No preset/drift model.
- "Create new profile" starts from FACTORY DEFAULTS (not a copy of current). Globals are
  preserved (`onboarded` stays 1 so onboarding never re-fires). A fresh profile therefore has
  zoom keys unbound; after creating one the UI scrolls to the keybind section as the obvious
  next step.

## Architecture: profile files + live mirror (chosen approach)

- Each profile is a sibling ini file: `profiles\<Name>.ini` in the same directory as the
  resolved `magnifier.ini` (repo dir in dev, `%LOCALAPPDATA%\Wind\profiles\` on the deployed
  Program Files build). Same flat `key=value` format, hand-editable.
- `magnifier.ini` stays the single live config both exes read and the core dir-watches. It
  gains ONE new key: `profile=<name>`.
- GLOBAL KEYS are never profile-scoped and survive every switch: `profile`, `onboarded`,
  `uiTheme`, `showAdvanced`. Everything else mirrors between the live ini and the active
  profile file.
- Rejected alternatives: `[profile:X]` sections inside magnifier.ini (breaks the flat parser
  and openIni hand-editing); pointer-only (core watches the profile file directly - breaks the
  single-file invariant everywhere for no user-visible gain).

## Switching

From either surface (tray or UI):
1. Read `profiles\<Name>.ini`.
2. Write its keys over `magnifier.ini`, preserving the global keys; set `profile=<name>`.
3. The core's existing dir-watch hot-reload applies everything hot-reloadable.
4. If the incoming profile's `model` differs from the RUNNING model, Wind must restart
   (model is not hot-swappable): the tray path relaunches Wind.exe via the existing
   single-instance eviction handshake; the UI path goes through the existing
   `restartWind` window-control. A tray balloon confirms the switch only when it forces a
   restart.

## Migration

Core side, at startup (in the LoadConfig I/O wrapper): if the profiles directory does not
exist, create `profiles\Default.ini` from the current live settings and write
`profile=Default` to the live ini. Existing installs become the "Default" profile with zero
user action. A `profile=` value naming a missing file behaves the same way as a corrupt file
on switch (see Error handling) at the next enumeration.

## Tray menu (Wind.exe, src/tray.cpp)

- New `Profiles` submenu inserted above "Open Settings".
- One radio-checked item per profile (checked = active), enumerated from the profiles dir at
  menu-open time, sorted case-insensitively, capped at 32 entries with menu IDs from a
  reserved range (no collision with existing IDs).
- Click = switch (see Switching). No management verbs in the tray; rename/duplicate/delete
  live in the settings UI only.

## Settings UI (ui/src)

Titlebar: the static "Wind Settings" caption becomes `Wind Settings - <Profile>` with a
chevron. The profile area is `app-region:no-drag` (the rest of the caption stays draggable).

- Click opens a dropdown: all profiles (active one checked), then a separator, then
  **Create new profile...** at the bottom.
- Right-click on a profile row (and a "..." button on the row, both wired to the same menu)
  opens: Rename / Duplicate / Delete.
- Create and Rename use a small inline name field with validation (empty, duplicate,
  forbidden characters); errors show inline, the commit button stays disabled while invalid.
- Duplicate copies the profile file to `<Name> copy` (then `<Name> copy 2`, ...) and does NOT
  switch to it.
- Delete confirms first. Deleting the ACTIVE profile switches to the first remaining profile.
  The last remaining profile cannot be deleted (Delete disabled with a hint).
- Switching or creating while edits are staged triggers the existing unsaved-changes guard
  (keep editing / discard and switch), reusing the `closePrompt` pattern from issue #164.
- Apply behavior: writes `magnifier.ini` exactly as today AND mirrors the profile-scoped keys
  into the active profile file. The live ini is authoritative; the profile file is re-mirrored
  on every Apply, so a missed mirror self-heals.

## Bridge / host (src/config_ui/main.cpp)

New bridge messages (host does all file ops; it already owns ini writing):

- `listProfiles` -> `{ type:'profiles', names:[...], active:'Name' }`
- `switchProfile { name }` -> refreshed `profiles` reply; goes through restartWind when the
  model differs from the running one
- `createProfile { name }` (factory defaults + preserved globals)
- `renameProfile { from, to }` (renaming the active profile also updates `profile=`)
- `duplicateProfile { name }`
- `deleteProfile { name }`

Each mutation replies with the refreshed list (plus `ok`/`error` for failures). All paths are
resolved relative to `wind::ResolveIniPath()` - never next to the exe (Program Files is
read-only for non-admin; see the standing gotcha).

## Profile names

- Name = file name (without `.ini`). Sanitized: forbidden characters `\ / : * ? " < > |`,
  leading/trailing whitespace trimmed, max 40 chars, non-empty, case-insensitive uniqueness,
  and Windows reserved device names (CON, PRN, AUX, NUL, COM1-9, LPT1-9) rejected.
- Validation is pure logic shared by create/rename/duplicate.

## Pure logic + tests

Following the existing pure/IO split (no `<windows.h>` in pure files):

- `src/profiles.h/.cpp` (pure): name sanitation/validation; the "merge profile keys over live
  config text, preserving global keys" transform; profile serialization round-trip; the
  "duplicate name -> `<Name> copy N`" generator. Doctest coverage for all of it, including:
  switch preserves globals, switch carries keybinds, migration seeds Default, rename of the
  active profile updates the pointer, delete-active falls back, last profile undeletable.
- I/O wrappers (enumerate/read/write/delete profile files) stay thin, in the host and in a
  small core-side helper for the tray.
- UI: Vitest for dropdown state where the suite already covers Settings (open/close, active
  check, create/rename validation states, guard interaction).

## Error handling

- Missing/corrupt profile file on switch: keep current settings, surface it (UI toast /
  tray balloon), drop the stale entry from the list on next enumeration.
- Failed file ops (locked, permissions): report in the UI with the reason; never a silent
  no-op.
- Two-writer note: both Wind.exe (tray switch) and WindConfig.exe (everything else) write
  `magnifier.ini`; that is already true today (swap-model relaunch vs. UI Apply) and writes
  are whole-file replaces, so last-writer-wins is acceptable; the profile mirror self-heals on
  the next Apply.

## Out of scope (YAGNI)

- Auto-switching profiles per foreground app.
- Import/export of profiles.
- Per-profile theme (uiTheme stays global).
- Tray-side profile management verbs.
