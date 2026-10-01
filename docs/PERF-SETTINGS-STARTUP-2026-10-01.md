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

## Caveats

- The commit subject of 9aa1c94 ("measured faster first paint") overstates the result: first paint did not
  improve. Only navigation completion did (about 30 ms).
- The numbers were captured with the code of commit 63b4075 (same lazy Onboarding and banner code as 9aa1c94;
  63b4075 only adds a comment to Banner.svelte). The "old" build is a hand-built variant of this branch with the
  static Onboarding import and eager banner, not a checkout of the real pre-Task-11 commit.

## Gate state (Task 11 fix-up)

- `build.bat test`: 407 doctest cases pass.
- `build.bat config`: builds, exit 0 (stop any running WindConfig.exe from the worktree first, it locks the link).
- `cd ui && npx playwright test`: 47 passed, 72 skipped, 0 failed, 6.5 s. `a11y.spec.js` and `settings.spec.js`
  target the pre-redesign UI and are skipped with `test.skip(true, ...)` until Task 12 rewrites them. They failed
  before Task 11 as well (66 failed, 6 passed), so this is not caused by the lazy-loading change. The earlier
  "52 vs 53 passed" difference came from those old-UI specs timing out and was not reproduced; with them skipped
  the count is stable.
