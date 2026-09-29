# Tinted pointer at 1x: implementation plan

**Spec:** `docs/superpowers/specs/2026-09-30-cursor-tint-design.md`. Branch `feat/288-colour-filters`.

1. `src/cursor_tint_pixels.h` (pure) + `tests/test_cursor_tint.cpp`: `TintArgb(pixels, n, matrix)` and
   `MonoToArgb(andBits, xorBits, w, h, out)` with the outline rule.
2. `src/cursor_tint.{h,cpp}`: `CursorTint` with `capture()`, `apply(matrix)`, `restore()`,
   `invalidate()`, `applied()`; builds tinted cursors with GetIconInfo/GetDIBits/CreateIconIndirect;
   logs each apply/restore with its duration.
3. `src/main.cpp`: capture after `RestoreInputState`; idle-tick `UpdateCursorTint` (colour, idle,
   magnify, fullscreen game every 250 ms); `restore()` at the start of `enterActive`; `invalidate()` on
   active -> idle; `WM_SETTINGCHANGE`/`SPI_SETCURSORS` recaptures; `restore()` at shutdown.
4. Build, unit + UI tests, deploy, field-verify per the spec, document in
   `docs/COLOUR-FILTER-FINDINGS.md`.
