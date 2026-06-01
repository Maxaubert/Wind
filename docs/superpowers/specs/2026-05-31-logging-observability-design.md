# Logging / Observability Design

**Status:** Approved (brainstorm). Issue #81.
**Date:** 2026-05-31

## Goal

Make customer-reported bugs diagnosable without reproducing them locally. Wind runs across
display configurations the author cannot fully test (resolution, HDR, refresh, rotation, DPI
scaling, VRR, GPU vendor/driver, PC specs). When a customer hits a bug or crash, a single
artifact they send back should contain enough to diagnose and fix it.

## Constraints

- **Local now, server later.** No network code today. The subsystem must produce a self-contained
  diagnostic bundle that is written/zipped locally now, and can later be POSTed to a server by
  swapping only the delivery step. The bundle format is the stable seam.
- **Zero hot-path cost.** Wind's value is a smooth, low-latency magnifier. Logging must add no
  per-frame work. This is the primary non-functional requirement.
- **Lean.** No third-party crash/telemetry SDK. Standard Win32 (`MiniDumpWriteDump`, DXGI/WMI
  queries) only. No new heavyweight dependency.
- **Program Files safe.** The deployed UIAccess build runs from `C:\Program Files\Wind` (read-only
  for the non-admin runtime), so all log/dump writes go to a per-user-writable location.

## Approach

Approach B from the brainstorm: a single unified logging module that all code uses, replacing the
three current ad-hoc loggers. Chosen over a minimal bolt-on (keeps the scattered ephemeral
`%TEMP%` logs, awkward server path) and over a third-party SDK (heavyweight, backend-oriented,
premature while local-only).

## Architecture

A new module `src/logging.{h,cpp}` exposing one API used everywhere:

```
namespace wind {
  enum class LogLevel { Info, Warn, Error };
  void LogInit(const wchar_t* processTag);   // "core" or "config"; resolves path, rotates, opens
  void Log(LogLevel lvl, const char* category, const char* fmt, ...);
  void LogShutdown();                         // flush + close
}
```

- Pure, testable helpers (line formatting, rotation decision, snapshot string assembly) live in a
  section compiled into the `WIND_TESTS` build with no `<windows.h>`. The Win32 I/O (file handle,
  path resolution, `MiniDumpWriteDump`, device/monitor queries) is excluded from the test build,
  matching the existing pure/Win32 split.
- Replaces `RLog` (render), `SiLog` (single-instance), and the always-on parts of `DiagLog`. The
  opt-in frame-pacing trace (`diagnostics=1`) keeps its own path and behaviour, unchanged.
- Both binaries link it. `Wind.exe` calls `LogInit(L"core")`, `WindConfig.exe` calls
  `LogInit(L"config")`. Each process writes its **own** file, so there is no cross-process file
  contention and no shared-handle locking.

## Performance design (primary requirement)

- **Event-driven only.** `Log()` is called on state transitions: startup, shutdown, zoom in/out
  begin/end, device-lost and recovery, monitor retarget, config hot-reload, HDR toggle, and on any
  warning/error. All are rare (human-scale or error-scale, not frame-scale).
- The per-frame tick loop and `renderFrame` contain **no** `Log()` calls. This is a hard rule and
  is grep-verifiable (no `Log(` inside the frame path).
- Net steady-state cost on the hot path: **zero**. The system snapshot runs once at startup; the
  crash handler only runs while already crashing.
- **Synchronous, buffered.** A single log file handle is kept open per process (not reopened per
  line as `RLog` does today). Writes are a formatted append plus a flush on `Warn`/`Error`. Because
  events are infrequent, this costs nothing measurable and needs no background thread. An async ring
  buffer is explicitly deferred (YAGNI) until/unless hot-path tracing is ever required.

## Log store

- Location: `%LOCALAPPDATA%\Wind\logs\` (the same per-user-writable root the ini already falls back
  to, via `ResolveIniPath`'s base; works in the Program Files deploy). Created on first run.
- Files: `wind-core.log` and `wind-config.log`.
- Format, one line per event:
  `2026-05-31T08:14:22.137Z  WARN  render   recreateDupl failed hr=0x887A0005`
  (UTC ISO-8601 timestamp with ms, fixed-width level, category, message). UTC avoids timezone
  ambiguity when reading a customer's log.
- **Rotation (bounded disk).** On `LogInit`, if the current file exceeds **1 MB** it is rotated:
  `wind-core.log` -> `wind-core.1.log` -> `wind-core.2.log`, keeping **3 generations** (~3 MB max
  per process). Crash artifacts (below) keep the **most recent 3** dump+summary pairs; older ones
  are deleted on startup. Total footprint is bounded and self-pruning, so a customer's disk never
  grows unbounded.

## System snapshot

Written once at the top of every session (right after `LogInit`), as a labelled block in the log:

- Wind version (from `VERSIONINFO`) and build flavour (normal vs uiaccess).
- OS: Windows build number + edition.
- CPU model, logical core count, total RAM.
- GPU: adapter description + driver version (per DXGI adapter; note the active adapter).
- Monitor topology, per monitor: device name, resolution, refresh rate, HDR enabled, rotation, DPI
  scaling percent, and VRR / adaptive-sync state where queryable.
- The active resolved `Config` (the same values the core is running with).

This block is the direct answer to "works on my machine": one glance shows the customer's exact
display and hardware reality.

## Crash handler

Extends the existing `SetUnhandledExceptionFilter` net (which today only restores the cursor) to
also, before the process dies, write into the log folder:

- A **minidump**: `wind-crash-<timestamp>.dmp` via `MiniDumpWriteDump` (normal + thread/handle
  data). Opens in Visual Studio / WinDbg against the matching PDB to give the exact faulting call
  stack.
- A **text summary**: `wind-crash-<timestamp>.txt` with the exception code, faulting module +
  offset, a register snapshot, and the system-snapshot block. Readable with no tools for a quick
  first look.

The handler does the minimum safe work (no allocation-heavy paths) since the process is already
unwinding a fault. Cursor restoration remains.

## Export + the server seam

- An **"Export diagnostics"** action: a tray menu item on `Wind.exe` and a button in the WindConfig
  settings UI. It zips the entire `%LOCALAPPDATA%\Wind\logs\` folder into
  `Wind-diagnostics-<timestamp>.zip` on the Desktop and reveals it in Explorer.
- The code is split into **produce-the-bundle** (gather + zip) and **deliver-the-bundle** (save to
  Desktop). The future server is a second delivery implementation (`POST` the same zip) selected at
  the call site. The bundle contents and layout are the stable contract; nothing else changes when
  the server arrives.
- Zip creation uses Windows' built-in PowerShell `Compress-Archive` (spawned via `CreateProcess`),
  not a vendored zip library. Decision made during implementation: it avoids adding a ~250 KB
  vendored dependency for a rare, user-initiated action, and the spawn cost (~300 ms, once per
  click) is irrelevant for a manual export. To work when both `Wind.exe` and `WindConfig.exe` are
  running (each holds its own log open), `ZipLogDir` first stage-copies the log files to a temp dir
  with `CopyFileW` (which can read a file held open with `FILE_SHARE_READ` by another process), zips
  that copy, then deletes it - so the live log handle is never closed and no lines are dropped. (The
  original plan specified a vendored `miniz` writer; this is the recorded deviation.)

## Build changes

- Enable PDB generation for the release/uiaccess builds (`/Zi` compile, `/DEBUG` link) and **archive
  the PDB per shipped version** (a minidump is only useful against its matching PDB). PDBs are not
  shipped to customers; they are kept by the author alongside each release.
- Add a `VERSIONINFO` resource (in the existing `src/wind.rc`) so the version appears in Explorer
  file properties and in every log line / crash report. A single source-of-truth version constant
  feeds both the resource and the snapshot.

## Scope

In scope: the logger module, the log store + rotation, the system snapshot, the crash handler
(minidump + summary), the export-to-zip action on both binaries, and the build changes
(PDB + VERSIONINFO). Applies to both `Wind.exe` and `WindConfig.exe`.

Out of scope (deliberately, for the server phase): any network/upload code, automatic collection,
consent/privacy UI, a third-party SDK, and any per-frame tracing beyond today's opt-in
`diagnostics=1` path.

## Testing

- **Unit (doctest, pure):** log-line formatting (timestamp/level/category layout), the rotation
  decision (size threshold, generation shifting, count cap), and snapshot string assembly from
  injected values. These compile into the existing `WIND_TESTS` build with no `<windows.h>`.
- **Manual / inspection (Win32):** force an unhandled exception (a gated self-test trigger) and
  confirm a dump + summary are written and the dump opens with a correct stack; force a device-lost
  and confirm the event is logged; trigger "Export diagnostics" and confirm the zip contains the
  logs + snapshot; verify the snapshot values against a known machine.
- Verify the no-per-frame-logging rule by grep over the frame path.

## Risks / notes

- `MiniDumpWriteDump` from inside a faulting process must avoid heap-heavy work; keep the handler
  minimal and pre-resolve the dump path at startup.
- VRR / adaptive-sync state is not uniformly queryable across drivers; capture it where available
  and record "unknown" otherwise rather than failing the snapshot.
- The deployed build is UIAccess (higher integrity); confirm the log folder and zip write succeed
  from that context (they target `%LOCALAPPDATA%`, which is writable, matching the ini path logic).
