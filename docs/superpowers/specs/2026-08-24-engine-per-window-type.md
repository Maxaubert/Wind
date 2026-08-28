# Per-window-type engine selection

Auto mode picks one engine per foreground window from a fixed heuristic. This exposes that choice,
and adds the two correctness rules the heuristic was missing - most importantly DRM content, which
the render engine cannot magnify at all.

## Why

Max, after watching the engine flap while switching between a terminal, a browser and Playnite:
"we need to improve this auto mode... better to have an advanced button on that settings page that
lets you set the model for each window type", and separately "we also need to handle drm content
switching somehow for netflix, apple tv etc".

The DRM half is not a preference. Desktop Duplication captures a copy-protected surface as BLACK,
so a render session over Netflix magnifies a black rectangle. Auto currently sends browsers to
render unconditionally (they are on `transformExclude` after crashing dwm.exe at high zoom over
Mica), which means Netflix in a browser is guaranteed to be broken today.

## Categories

Classified by `ClassifyWindow` (pure, in `engine_pick.h`) from signals already read per tick:

| Category | Signal | Notes |
|---|---|---|
| `game` | borderless AND covers the monitor | Games, F11 video. Beats `acrylic` when both apply. |
| `acrylic` | `DWMWA_SYSTEMBACKDROP_TYPE` in {mica, acrylic, tabbed} | See the honesty note below. |
| `desktop` | existing `IsShellDesktopFg` | Win+D / Progman. Never a game, even though it reads as a borderless cover (issue #172). |
| `other` | everything else | |

Each maps to an ini key (`engineGame`, `engineAcrylic`, `engineDesktop`, `engineOther`) taking
`auto|transform|render`, all defaulting to `auto`. **An untouched install behaves exactly as
before** - that is the safety property, and `auto is byte-for-byte the old behaviour` in
`test_engine_pick.cpp` is what holds it.

**Honesty note on the acrylic bucket.** Probed against every visible window on the dev machine:
the backdrop attribute is readable cross-process on 13 of 13, but 8 of them report `DWMSBT_AUTO`,
which means "the system decides" rather than "no backdrop". A third-party app painting its own
blur never appears at all. So the bucket catches windows that OPT IN - a subset of what looks
blurred on screen. It is named after the signal for that reason, and the UI copy says "windows that
ask for a Mica or acrylic background".

## The two rules that outrank the user

Shown in the UI as fixed, not editable:

1. **Capture-protected content never gets render.** `GetWindowDisplayAffinity != WDA_NONE` on the
   foreground **or any descendant**. The child walk is the point: Netflix or Apple TV inside a
   browser leaves the top-level frame unprotected and marks only the video surface, so checking the
   foreground alone misses exactly the case this exists for. Our own overlay is capture-excluded by
   design (the feedback-loop guard) and was the only protected window in the probe, so windows
   belonging to this process are skipped or we would detect ourselves and pin the engine forever.
2. **`transformExclude` apps never get transform.** Unchanged.

### The conflict, and how it resolves

Netflix inside a browser fires both. **Protected wins.** Black video every single time is a worse
failure than a rare crash risk that the pan wall and MPO buster already mitigate. Signed off by Max
on 2026-08-24; `Netflix in a browser: protected beats transformExclude` pins the ordering.

`renderExclude` (exe list, mirrors `transformExclude`) is the manual escape hatch for protected apps
the affinity probe misses.

## Cost

Both new detections are cached per foreground HWND alongside the existing exe-derived predicates.
The mid-zoom pick site runs EVERY zoomed tick, and the protection check walks child windows - a
browser has plenty - so doing it at 144Hz would be real waste for a value that can only change when
the foreground does.

## Known gap

The display-affinity detection is **unverified against real DRM content** - no protected app was
running when it was built, and the probe's only hit was Wind's own overlay. If it misses, the
symptom is unchanged-from-today (black video on render) and `renderExclude` is the workaround.
Needs a hands-on check with Netflix or Apple TV before this is trusted.
