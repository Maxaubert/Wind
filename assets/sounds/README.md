# Wind chimes

The enable / disable sounds for the tray flyout's listen-and-chime chips (issue #315). They are
synthesized, not recorded: no Windows system sound and no third-party pack is used.

| File | Meaning |
| --- | --- |
| `chime_on.wav` | Enabled: two soft notes rising (E5 then A5, a fourth), peak -6 dBFS |
| `chime_off.wav` | Disabled: the same notes falling, a little softer, peak -8 dBFS |
| `candidates/` | Three alternative pairs (glassy, marimba, airy pluck) and `index.html` to audition them |

All are 48 kHz, 16-bit, mono, 0.46 s, with a 4 ms attack, a 60 ms fade-out and a light echo tail.

## Regenerate

    node tools/make_chimes.mjs           write every WAV and the preview page
    node tools/make_chimes.mjs --check   only verify the committed WAVs

Needs only Node, no network and no packages. Output is deterministic (the noise is seeded), so
re-running produces identical bytes. The script verifies each file after writing it (format, no
clipping, no DC offset, no click at either end, duration 0.35 to 0.6 s). `tests/test_chime_wav.cpp`
runs the same rules on the committed files in `build.bat test`.

## Change the sound

Edit `VOICES` (timbre) and `SETS` (notes, timing, echo) in `tools/make_chimes.mjs`, regenerate, and
open `candidates/index.html` to listen. To ship a candidate as the default, point `SETS.default` at its
voice and notes, or copy its two WAVs over `chime_on.wav` / `chime_off.wav`.

## In the app

`WindTray.exe` embeds both files as `WAVE` resources (`src/tray_app/wind_tray.rc`) and plays them with
`PlaySound(SND_RESOURCE | SND_ASYNC | SND_NODEFAULT)` through `TrayApp::PlayChime(bool enabled)`
(`src/tray_app/chime.cpp`).
