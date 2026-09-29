# Colour filters (issue #288)

Date: 2026-09-29. Owner: Max. Status: awaiting approval (spec + plan together).

## 1. What the owner asked for

Colour filters for low vision, chosen in Settings and optionally toggled with a hotkey:

- **Invert colours**
- **Greyscale**
- **Custom tints**: two-colour reading schemes (yellow on black, white on blue, green on black)
- **Warm (orange) tint**, like a night light, with a strength setting
- **Dim**: an artificial "TV brightness" control that darkens the picture (the panel's backlight is
  untouched; it is a colour-scale on the image)

Decisions (2026-09-29):
1. Filters apply **while zoomed and at 1x**.
2. A **toggle hotkey** is optional; a filter can simply be always on.
3. Group the choices in dropdowns rather than a long list of toggles.

## 2. Behaviour

- Settings > **Colour** section:
  - **Colour filter** (dropdown): Off, Invert, Greyscale, Warm, Yellow on black, White on blue,
    Green on black.
  - **Warm strength** (slider 10-100%, default 50%): used by Warm.
  - **Dim** (slider 100% = off down to 20%, default 100%): composes with any filter (e.g. Invert +
    dim), and works on its own with the filter Off.
  - **Also when not zoomed** (toggle, default on): off = the filter only shows while zoomed.
  - **Toggle colour filter** (keybind, optional): flips the filter (and dim) on and off without
    changing the chosen settings. The toggle state resets to "on" when Wind starts.
- The filter covers the whole monitor Wind magnifies, including the cursor, exactly like Windows
  Magnifier's colour inversion.
- `model=magnify` (native Windows Magnifier) is out of scope: Windows Magnifier has its own filters;
  the rows show a note there.
- Nothing changes for a user with no filter and dim at 100%: no context, no cost (see 4.3).

## 3. Scope

In: the five filter kinds above, dim, the 1x option, the hotkey, both engines on the primary
monitor. Out (v1): per-app filters (can ride #286 profiles later), colour-blind correction matrices,
custom colour pickers for tints, multi-monitor secondaries at 1x.

## 4. Design

### 4.1 Mechanism: the DWM colour effect

`MagSetFullscreenColorEffect(MAGCOLOREFFECT*)` (Magnification.dll, the same call Windows Magnifier
uses for inversion) applies a 5x5 colour matrix inside DWM to everything composed on screen. It costs
nothing per frame (done by the compositor) and works at level 1. It needs a live magnification
runtime (`MagApiAcquire`) and, like every Magnification call, must run on the runtime's owner thread
(`MagThreadInvoke`).

One effect for both engines: the render engine's overlay is itself a window DWM composes, so the
effect lands on the magnified picture too. **This must be verified by the spike (Task 1)**: if Desktop
Duplication captures the desktop AFTER the effect, the render engine would filter twice, and the
render path then needs the effect cleared while its overlay is up and the matrix applied in its pixel
shader instead (the shader already has a brightness stage, `render_shaders.h`).

### 4.2 Units

| Unit | Kind | Responsibility |
|---|---|---|
| `src/color_matrix.h` | pure, doctested | Build the 5x5 matrix from (filter, warm strength, dim); identity check; compose |
| `src/color_filter.h/.cpp` | Win32 | Holds a runtime reference while a filter must exist at 1x, applies the matrix (deduped) through the owner thread, restores identity and releases on disable / shutdown / crash |
| `src/main.cpp` RunTick | integration | Decide the wanted matrix per tick (config, toggle state, zoomed, model) and hand it to the controller; the toggle hotkey |
| `src/config.*`, `ui/src/settings-schema.js` | settings | Keys, defaults, the Colour section |

### 4.3 Runtime lifetime and the 1x cost

- Zoomed: the engine already holds a runtime; the filter adds nothing.
- At 1x with a filter on: the controller holds its own `MagApiAcquire` reference, so the transform
  model's idle release (~1.2 s after zoom-out) does not tear the context down. **Cost, by design:** a
  live context adds a DWM re-composite to every cursor shape/visibility change any app makes (the
  documented tax; a game that toggles its pointer can hitch). It is paid ONLY while a 1x filter is on;
  "Also when not zoomed" off avoids it.
- Filter off (or dim 100% with filter Off, or toggled off): identity is written and the reference is
  released, so the idle machine is exactly as today.

### 4.4 Safety

- Identity is restored on every exit path: disable, toggle, model switch, `shutdown`, the crash
  filter and `atexit` (next to the existing `RestoreInputState`).
- The spike checks whether Windows clears the effect by itself when the process dies. If it does not,
  a killed Wind would leave the screen filtered; the crash-path restore is then mandatory, and a
  stale effect found at startup is cleared.

### 4.5 Matrices (row vectors, DWM layout: out = in x M)

- Invert: `-1` diagonal, `+1` offset row (on RGB).
- Greyscale: luma weights 0.2126 / 0.7152 / 0.0722 in every column.
- Warm (strength s): R x 1, G x (1 - 0.25 s), B x (1 - 0.6 s).
- Two-colour tints: greyscale luma L, then out = bg + L x (fg - bg) for dark-background schemes
  (text light) computed from the INVERTED luma so dark text on white pages becomes light on dark:
  yellow on black (fg 1,1,0 / bg 0,0,0), white on blue (fg 1,1,1 / bg 0,0,0.5), green on black
  (fg 0,1,0 / bg 0,0,0).
- Dim d (0.2-1.0): RGB x d. Composed last with the chosen filter.

## 5. Testing

- Doctests for every matrix (identity, invert of white/black, greyscale weights, warm strength ends,
  tint endpoints, dim composition, identity detection).
- Playwright for the Colour section rows.
- Spike + field on this PC (signed UIAccess build): each filter at 1x and zoomed, in both engines
  (transform desktop, render via `desktopTransform=0`), HDR on/off, hotkey toggle, zoom in/out
  transitions with no flash, Wind quit and kill leave the screen clean, no hitch at 1x with no filter.

## 6. Delivery

One PR: issue #288 -> `feat/288-colour-filters`, version bump inside the PR (0.12.0 as a feature;
owner may prefer a patch bump), release on merge.

## Amendment: two sliders only (owner field test, 2026-09-29)

This supersedes the filter list, the "also when not zoomed" toggle and the hotkey above.

- The feature is two sliders, always applied (zoomed and at 1x): **Warmth** (`colorWarmPct`,
  0-100, default 0 = off) and **Brightness** (`colorDimPct`, 0-100, default 100). Both neutral = no
  colour effect and no held Magnification runtime.
- Warmth follows the blackbody curve from 6500 K to 1200 K (Night light's range), linear in
  mireds; 100% is about (1, 0.34, 0). The first version (G x 0.75, B x 0.4 at 100%) read as dim pink.
- Brightness floor 1% (`kMinDim01`, owner request; not 0, a black screen looks like a dead display).
- Removed: Invert, Greyscale, the two-colour tints, `colorAt1x`, the toggle hotkey
  (`colorFilter`, `colorAt1x`, `colorToggleVk/Mods` in an old ini are ignored). Why: a single
  colour matrix cannot keep multi-coloured text readable, see `docs/COLOUR-FILTER-FINDINGS.md`.
