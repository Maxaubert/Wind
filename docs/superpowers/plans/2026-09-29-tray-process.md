# Tray Process Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The tray icon and menu live in a non-UIAccess WindTray.exe so the menu stacks like any app's menu (below the cursor and the Snipping Tool overlay).

**Architecture:** Wind.exe creates a named shared-memory block with its live status and starts WindTray.exe; the helper owns the icon and menu, reads the block, writes `menuOpen`, and acts through files, ShellExecute and the existing quit event.

**Tech Stack:** C++17 / MSVC, Win32 (Shell_NotifyIcon, file mapping), doctest, NSIS.

**Spec:** `docs/superpowers/specs/2026-09-29-tray-process-design.md`

## Global Constraints

- No em-dashes anywhere. Pure headers do not include `<windows.h>`.
- WindTray.exe must NOT have a uiAccess manifest; Wind must start it with ShellExecuteW (verified: children do not inherit UIAccess).
- The user-visible tray (icon, tooltip, menu, header, balloons) stays identical.
- Quit goes through `Local\Wind_QuitRequest`, never a kill.
- Branch `feat/291-tray-process`, commits `type(scope): subject` + trailer, patch version bump.

## Review Focus

1. Explorer restart while Wind runs: the icon comes back once, never twice.
2. Wind killed (Task Manager): the tray icon disappears (no ghost icon) and WindTray exits.
3. WindTray killed: Wind restarts it; a helper that crashes on start does not spin.
4. Two Wind instances (the eviction handshake on a model relaunch): exactly one tray icon at the end.
5. The menu open while Wind's tick reads `menuOpen`: the weld stays suspended exactly as today.

---

### Task 1: Shared block and status transport

**Files:** Create `src/tray_ipc.h` (pure layout + version check), `tests/test_tray_ipc.cpp`; modify `src/tray_status.h` (keep labels; add conversion to/from the block), `src/main.cpp` (create the mapping at startup, publish into it each tick instead of the atomics).
- [ ] Tests for the layout check (magic/version mismatch -> "no live data") and the status round trip.
- [ ] Wind creates `Local\Wind_TrayState_v1`, writes `windPid`, publishes level/engine/panning.
- [ ] Decide the diagnostics-export balloon path (shared notify seq/text vs WindConfig-side); implement the smaller.
- [ ] Commit `feat(tray): shared status block (#291)`.

### Task 2: WindTray.exe

**Files:** Create `src/tray_app/main.cpp`, `src/tray_app/WindTray.manifest` (asInvoker, PMv2); move `src/tray.cpp` owner-draw/menu/profile code into the helper (shared sources compiled into both where needed); `build.bat` target.
- [ ] Single instance mutex, icon add/remove, `TaskbarCreated` re-add, wait on Wind's process handle -> remove icon and exit.
- [ ] Menu thread as today, header reads the block; `menuOpen` written around TrackPopupMenu.
- [ ] Actions: Settings (WindConfig from own folder), Quit (quit event), Profiles (SwitchToProfile; engine change relaunches Wind.exe from own folder).
- [ ] Commit `feat(tray): WindTray.exe helper owns the tray icon and menu (#291)`.

### Task 3: Wind.exe side

**Files:** `src/main.cpp`, `src/tray.cpp/.h` (removed or reduced).
- [ ] Remove the tray icon/menu from Wind; start WindTray with ShellExecuteW + PID; restart it if it exits (<= 3 per minute); read `menuOpen` for `suppressCursorSync`.
- [ ] Commit `refactor(tray): Wind.exe no longer owns the tray (#291)`.

### Task 4: Packaging

**Files:** `tools/uiaccess_setup.ps1`, `installer/wind.nsi`, `tools/installer_check.ps1`, `.github/workflows/release.yml` / `alpha.yml` if they list files.
- [ ] Build, deploy, install, uninstall and upgrade include WindTray.exe; installer_check asserts it.
- [ ] Commit `build(tray): ship WindTray.exe (#291)`.

### Task 5: Verify, docs, ship

- [ ] Unit tests, build, deploy; owner field test per spec section 6 (owner runs every UI check).
- [ ] Docs: CLAUDE.md (three binaries; the UIAccess stacking reason), `docs/architecture/` process section, installer README.
- [ ] Review workflow, owner approves fixes, PR, owner says merge.
