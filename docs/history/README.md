# Closed investigations

Frozen records of investigations that are closed. They stay for their measurements and their "do
not re-try" conclusions; the code and [the architecture docs](../architecture/README.md) are
authoritative, and these files are not updated as the code moves on.

- [NATIVE-MAGNIFIER-STOMP.md](NATIVE-MAGNIFIER-STOMP.md): a running built-in Magnifier fights the transform engine; the stomp guard (#217; picking Render while it runs is parked).
- [PERF-ACRYLIC-PARITY-2026-08-21.md](PERF-ACRYLIC-PARITY-2026-08-21.md): zoom-in stalls over acrylic versus the built-in Magnifier; the `txMaxStepPct` cap (#219).
- [POINTER-HITTEST-FINDINGS.md](POINTER-HITTEST-FINDINGS.md): hover dead zones on the transform desktop; the source-rect input transform (#185).
- [SHELL-PANEL-CURSOR-FINDINGS.md](SHELL-PANEL-CURSOR-FINDINGS.md): the sprite cursor under the emoji picker and other shell panels; the pointer freeze, since removed (#283).
