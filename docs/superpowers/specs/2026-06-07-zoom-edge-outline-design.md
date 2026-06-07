# Zoom edge outline - design

Date: 2026-06-07
Status: approved (pending user spec review)

## Problem

When zoomed in only a small amount (e.g. 1.2x-1.5x), it can be hard to tell at a glance
that the magnifier is active - the view looks almost like the normal desktop. Wind needs a
clear, always-visible indicator that zoom is on. A solid outline around the screen edges
makes the zoomed state unmistakable, and is most valuable exactly in the low-zoom case.

## Behavior

- While zoomed (`level > 1.0`), draw a solid rectangular outline hugging the four screen
  edges of the magnified monitor.
- The overlay is already alpha-0 (invisible) at 1x and only revealed while a zoom session is
  active, so the outline appears/disappears naturally with the session. The `level > 1.0`
  gate in the draw is belt-and-braces (and also suppresses the outline if a frame is ever
  drawn at exactly 1x while visible).
- Default state: **off**. When enabled, the default style is **4px solid, accent color
  `#5b5bd6`, fully opaque**.
- Thickness is expressed in **physical pixels** (matches how the renderer already works; no
  DPI scaling). Configurable 1-40 px.
- Color is configurable as a hex RGB string; the config UI exposes a native color picker.

This is purely a visual indicator. It does not affect capture, panning, clicks, the cursor,
or zoom math.

## Rendering

All in the own GPU renderer (`render_engine.cpp` + `render_shaders.h`). The outline draws
into the existing capture-excluded overlay (`WDA_EXCLUDEFROMCAPTURE`), so it can never feed
back into Desktop Duplication.

### Shader (`render_shaders.h`)

Add a minimal solid-color shader `kBorderHLSL`:

- Constant buffer (32 bytes): `float2 posClip; float2 sizeClip; float4 color;`
- VS: identical quad expansion to the cursor shader (`id & 1`, `(id >> 1) & 1` ->
  `(0,0),(1,0),(0,1),(1,1)`), placing a quad at `posClip + q * sizeClip`. Drawn as a
  4-vertex triangle strip.
- PS: returns the constant `color` (alpha included). No texture, no sampler.

### Draw pass (`State::render`)

A new `RenderEngine::State` block draws the outline after the magnify pass and before the
cursor pass, so the outline overlays the magnified image but the cursor still draws on top.

- Gate: `p.outline && p.level > 1.0 && haveDesktop`.
- Thickness `t = clamp(p.outlineThicknessPx, 1, min(sw, sh) / 2)` (never let the four edge
  quads overlap/invert on a tiny target).
- Four quads in physical pixels, converted to clip space with the same mapping the cursor
  pass uses (`posClipX = x/sw*2-1`, `posClipY = 1 - y/sh*2`, `sizeClipX = w/sw*2`,
  `sizeClipY = -(h/sh*2)`):
  - top:    x=0,      y=0,      w=sw,      h=t
  - bottom: x=0,      y=sh - t, w=sw,      h=t
  - left:   x=0,      y=t,      w=t,       h=sh - 2t
  - right:  x=sw - t, y=t,      w=t,       h=sh - 2t
- Opaque (blend state `nullptr`, the same opaque state the magnify pass uses) for crisp
  edges. The color's alpha is written but with blend off it is effectively 1.0; keeping it
  in the cb leaves the door open for a future opacity knob without a shader change.
- Reuses one new constant buffer (`bcb`, 32 bytes), updated once per edge via
  `UpdateSubresource` then `Draw(4,0)`. Four trivial draws per frame, only while zoomed.

### New device resources (`buildDeviceResources` / recovery)

- `bvs` (vertex shader), `bps` (pixel shader), `bcb` (constant buffer). Compiled/created
  alongside the cursor pipeline, and released + rebuilt in `recoverDeviceLost()` exactly like
  the other device-dependent resources (so the two paths can't drift).

### New `RenderFrameParams` fields (`render_engine.h`)

```cpp
bool  outline;             // draw the edge outline while zoomed
int   outlineThicknessPx;  // physical px, clamped in render()
float outlineR, outlineG, outlineB;  // 0..1, written straight to the BGRA8 backbuffer so the
                                     // stored value matches the sRGB hex the user picked (no
                                     // linearization; the magnify pass writes sRGB-encoded pixels too)
```

## Config

### Struct + parse (`config.h`, `config.cpp`)

New fields on `Config`:

```cpp
int         outline          = 0;          // 0/1, off by default
int         outlineThickness = 4;          // physical px
std::string outlineColor     = "5b5bd6";   // hex RGB, optional leading '#'
```

- `ParseConfig`: add `outline`, `outlineThickness` (stoi), `outlineColor` (string).
- Clamp `outlineThickness` to `[1, 40]` in the clamp block.
- `outlineColor` is stored as-is; validation/parsing to RGB happens at use (a malformed value
  falls back to the accent default, see below). Keep an optional leading `#` tolerated.

### Hex parsing helper

A small pure helper (testable, no `<windows.h>`) converts a hex color string to three floats
in 0..1:

```cpp
// Parse "#rrggbb" or "rrggbb" -> r,g,b in [0,1]. Returns false (and leaves outputs untouched)
// on any malformed input, so the caller keeps its fallback default.
bool ParseHexColor(const std::string& s, float& r, float& g, float& b);
```

Lives next to the config parse logic (pure half). Used by `FillRenderParams`.

### Wiring (`main.cpp` `FillRenderParams`)

```cpp
p.outline = (cfg.outline != 0);
p.outlineThicknessPx = cfg.outlineThickness;
float r = 0.357f, g = 0.357f, b = 0.839f;   // #5b5bd6 fallback
ParseHexColor(cfg.outlineColor, r, g, b);
p.outlineR = r; p.outlineG = g; p.outlineB = b;
```

### Default-ini template (`config.cpp` `LoadConfig`)

Append documented keys to the written-defaults block:

```
; outline: 1 = draw a solid outline around the screen edges while zoomed (an at-a-glance
;   "you are zoomed" indicator, handy at low zoom); 0 = off (default)
outline=0
; outlineThickness: outline width in pixels (1-40)
outlineThickness=4
; outlineColor: outline color as hex RGB (e.g. 5b5bd6 = Wind accent; leading # optional)
outlineColor=5b5bd6
```

## Config UI

The WebView2 bridge is generic - `getConfig` enumerates every ini key/value and `setConfig`
writes any key (`UpdateIniText`). No host (`config_ui/main.cpp`) or `ini_edit` changes are
needed; new keys round-trip automatically. Only the Svelte layer changes.

### New `color` row type (`ui/src/lib/Row.svelte`)

Add a branch:

```svelte
{:else if row.type === 'color'}
  <input class="color" type="color" {disabled} value={value}
         on:input={e => onChange(e.target.value)} />
```

`<input type="color">` always yields `#rrggbb` (lowercase), which is exactly what the ini
stores. A small style block matches the control to the UI (rounded swatch, themed border).

### Schema rows (`ui/src/settings-schema.js`, Display section)

```js
{ key:'outline',          type:'toggle', label:'Edge outline',
  desc:'Show an outline around the screen while zoomed.', def:0 },
{ key:'outlineThickness', type:'slider', label:'Outline thickness',
  desc:'Width in pixels.', min:1, max:40, step:1, def:4, dependsOn:'outline' },
{ key:'outlineColor',     type:'color',  label:'Outline color',
  def:'#5b5bd6', dependsOn:'outline' },
```

`dependsOn:'outline'` greys the thickness/color rows out when the outline is off (the same
mechanism `smoothZoom*` rows use). The default `#5b5bd6` carries the leading `#`; the C++
parser tolerates it.

## Testing

- **Unit (doctest):** `ParseHexColor` - valid 6-digit with and without `#`, uppercase,
  malformed (wrong length, non-hex chars) returns false and leaves outputs untouched, and the
  accent default `5b5bd6` maps to roughly (0.357, 0.357, 0.839).
- **Config parse:** a `ParseConfig` case asserting `outline`, `outlineThickness` (incl. clamp
  past 40), and `outlineColor` round-trip.
- **Visual:** the overlay is capture-excluded, so external screenshots can't see it. Verify
  in-app via `WIND_SELFTEST=1 Wind.exe` (dumps `wind_selftest.png`) with the outline enabled,
  confirming a solid accent frame at the four edges, plus a manual low-zoom check.
- **Build:** `build.bat test` (core + doctest), then `build.bat config` for the UI/host.

## Out of scope (YAGNI)

- Opacity / transparency knob (the cb carries alpha, so it's a cheap future add if wanted).
- Glow / gradient / animated pulse styles.
- Rounded corners.
- Per-monitor or per-edge customization.
- Showing the outline only below a zoom threshold (decided: always-on while zoomed).
