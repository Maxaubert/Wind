# Settings window start-up measurement (#303, Task 11)

Measured 2026-10-01 on the dev box (9950X3D, RTX 5090, warm WebView2 runtime), release `WindConfig.exe`
from `build.bat config`, launched from a normal shell, killed after 4 s, 11-12 launches per build,
alternating old/new. Numbers come from the host's own log lines in `logs\wind-config.log`:

    startup: navigation completed N ms after launch
    startup: first paint N ms after launch      (page posts "ready" two animation frames after App mount)

Old build = this branch with the `ready` post and host logging, but with the static Onboarding import and
the eager banner image (the pre-Task-11 behaviour). New build = HEAD (lazy Onboarding chunk, banner image
set two frames after mount).

| Build | navigation completed (median) | first paint (median) | first paint (range) |
|-------|-------------------------------|----------------------|---------------------|
| Old   | 344 ms                        | 407 ms               | 390-719 ms          |
| New   | 313 ms                        | 407 ms               | 390-469 ms          |

Raw first paint, old: 407 390 406 422 469 391 390 406 719 390 422. New: 390 438 407 469 406 391 407 407 406 391 406.
Raw navigation, old: 344 328 344 360 391 329 328 328 719 328 329. New: 312 344 329 344 312 297 328 313 313 313 313.

## Reading

- Settings launches (the normal case) never use Onboarding, so lazy-loading it removes parse work from the
  main bundle: navigation completes about 30 ms sooner. First paint is unchanged (median 407 ms for both),
  so first paint is dominated by WebView2 start-up, not by our bundle.
- The banner image is requested two frames after mount, about when `ready` is posted. That keeps it off the
  first render's critical path, but it is not a measurable gain; do not claim one.
- Honest summary: a modest (about 30 ms) navigation gain, no first-paint change. The onboarding chunk is 9.65 kB.
- `ready` is posted when App mounts, not when the session has loaded, so "first paint" means the shell's first
  paint, not fully populated settings.

## Gate state at this commit (Task 11)

- `build.bat test`: 407 doctest cases pass (re-run from PowerShell as `.\build.bat test`; `build.bat` is not
  found from the bash shell because the cwd is not on its search path).
- Playwright: `a11y.spec.js` and `settings.spec.js` still target the old UI and fail on the pre-Task-11 UI
  as well (66 failed, 6 passed in those two files on a build without the lazy changes). They are rewritten in
  Task 12. keybind-rules, onboarding, schema, search, session and shell all pass.
- Stability: two repeat runs of those two files at HEAD gave the identical 6 passed / 66 failed with the
  identical set of passing titles; the other six files give 47 passes. 47 + 6 = 53 passing of 119 listed, and
  that count was identical in every run I made, so I could not reproduce a 52. The earlier 52-vs-53 most likely
  came from the old-UI specs, where each failure waits out a 30 s timeout (so a full `npx playwright test`
  exceeds 600 s right now); it is not tied to the lazy Onboarding import, and no conditional or skipped
  tests exist (grep for skip/fixme/only/env gates finds none).
