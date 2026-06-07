# Zoom Edge Outline Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add an optional solid outline around the screen edges that appears while the magnifier is zoomed, as an at-a-glance indicator that zoom is active (most useful at low zoom).

**Architecture:** A new solid-color quad shader draws four thin edge quads into the existing capture-excluded D3D11 overlay, after the magnify pass, gated on `level > 1.0`. Three new hot-reloadable config keys (`outline`, `outlineThickness`, `outlineColor`) drive it; a pure `ParseHexColor` helper converts the hex string to float RGB. The config UI exposes them in the Display section via a new native color-picker row type. The generic WebView2 bridge needs no changes.

**Tech Stack:** C++17 / MSVC, Direct3D 11 + HLSL, doctest (vendored), Svelte + Vite (config UI).

**Spec:** `docs/superpowers/specs/2026-06-07-zoom-edge-outline-design.md`

---

## File Structure

- `src/config.h` - declare `ParseHexColor`; add `outline`, `outlineThickness`, `outlineColor` to `Config`.
- `src/config.cpp` - implement `ParseHexColor` (pure section); parse + clamp the new keys; add them to the default-ini template.
- `tests/test_config.cpp` - unit tests for `ParseHexColor` and the new config keys.
- `src/render_shaders.h` - add `kBorderHLSL` solid-color shader.
- `src/render_engine.h` - add the new `RenderFrameParams` fields.
- `src/render_engine.cpp` - border device resources (build + recovery) and the draw pass.
- `src/main.cpp` - wire config -> params in `FillRenderParams`.
- `ui/src/lib/Row.svelte` - new `color` row type.
- `ui/src/settings-schema.js` - the three Display rows.

The test build (`build.bat test`) compiles `src/config.cpp` with `WIND_TESTS`, so `ParseHexColor` MUST live in the pure section of `config.cpp` (above the `#ifndef WIND_TESTS` I/O block) and be declared in `config.h`. The render-engine code is Win32/D3D and is verified by build + the in-app self-test, not unit tests.

---

## Task 1: `ParseHexColor` pure helper (TDD)

**Files:**
- Modify: `src/config.h` (declaration near `ParseConfig`)
- Modify: `src/config.cpp` (implementation in the pure section, before `#ifndef WIND_TESTS`)
- Test: `tests/test_config.cpp`

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_config.cpp`:

```cpp
TEST_CASE("ParseHexColor parses 6-digit hex with and without leading #") {
    float r = -1, g = -1, b = -1;
    CHECK(ParseHexColor("#5b5bd6", r, g, b) == true);
    CHECK(r == doctest::Approx(91.0f / 255.0f));   // 0x5b
    CHECK(g == doctest::Approx(91.0f / 255.0f));   // 0x5b
    CHECK(b == doctest::Approx(214.0f / 255.0f));  // 0xd6

    float r2, g2, b2;
    CHECK(ParseHexColor("ffffff", r2, g2, b2) == true);
    CHECK(r2 == doctest::Approx(1.0f));
    CHECK(g2 == doctest::Approx(1.0f));
    CHECK(b2 == doctest::Approx(1.0f));

    float r3, g3, b3;
    CHECK(ParseHexColor("FF0000", r3, g3, b3) == true);   // uppercase
    CHECK(r3 == doctest::Approx(1.0f));
    CHECK(g3 == doctest::Approx(0.0f));
    CHECK(b3 == doctest::Approx(0.0f));
}
TEST_CASE("ParseHexColor rejects malformed input and leaves outputs untouched") {
    float r = 0.5f, g = 0.5f, b = 0.5f;
    CHECK(ParseHexColor("", r, g, b) == false);
    CHECK(ParseHexColor("#", r, g, b) == false);
    CHECK(ParseHexColor("12345", r, g, b) == false);     // too short
    CHECK(ParseHexColor("1234567", r, g, b) == false);   // too long
    CHECK(ParseHexColor("gggggg", r, g, b) == false);    // non-hex
    CHECK(r == doctest::Approx(0.5f));                    // unchanged on failure
    CHECK(g == doctest::Approx(0.5f));
    CHECK(b == doctest::Approx(0.5f));
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `build.bat test`
Expected: FAIL to compile/link with `ParseHexColor` undeclared/unresolved.

- [ ] **Step 3: Declare in `src/config.h`**

Add directly below the `Config ParseConfig(const std::string& text);` declaration:

```cpp
// Pure: parse "#rrggbb" or "rrggbb" (case-insensitive) into r,g,b floats in [0,1]. Returns
// false on any malformed input (wrong length, non-hex), leaving the outputs untouched so the
// caller keeps its fallback default.
bool ParseHexColor(const std::string& s, float& r, float& g, float& b);
```

- [ ] **Step 4: Implement in `src/config.cpp`**

Add inside `namespace wind {`, in the pure section (e.g. just after the `trim` helper, before `ParseConfig`):

```cpp
bool ParseHexColor(const std::string& s, float& r, float& g, float& b) {
    size_t i = (!s.empty() && s[0] == '#') ? 1 : 0;
    if (s.size() - i != 6) return false;
    auto hexv = [](char ch, int& out) -> bool {
        if (ch >= '0' && ch <= '9') { out = ch - '0'; return true; }
        if (ch >= 'a' && ch <= 'f') { out = ch - 'a' + 10; return true; }
        if (ch >= 'A' && ch <= 'F') { out = ch - 'A' + 10; return true; }
        return false;
    };
    int v[6];
    for (int k = 0; k < 6; ++k) if (!hexv(s[i + k], v[k])) return false;
    r = (v[0] * 16 + v[1]) / 255.0f;
    g = (v[2] * 16 + v[3]) / 255.0f;
    b = (v[4] * 16 + v[5]) / 255.0f;
    return true;
}
```

- [ ] **Step 5: Run tests to verify they pass**

Run: `build.bat test`
Expected: PASS (exit 0), all `ParseHexColor` cases green.

- [ ] **Step 6: Commit**

```bash
git add src/config.h src/config.cpp tests/test_config.cpp
git commit -m "feat(config): add ParseHexColor pure helper (#92)"
```

---

## Task 2: Config fields, parse, and clamp (TDD)

**Files:**
- Modify: `src/config.h` (`Config` struct)
- Modify: `src/config.cpp` (`ParseConfig` key handling + clamp block)
- Test: `tests/test_config.cpp`

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_config.cpp`:

```cpp
TEST_CASE("outline keys default off with accent color") {
    Config c = ParseConfig("");
    CHECK(c.outline == 0);                  // off by default
    CHECK(c.outlineThickness == 4);
    CHECK(c.outlineColor == "#5b5bd6");     // Wind accent
}
TEST_CASE("outline keys parse and thickness clamps to [1,40]") {
    Config c = ParseConfig("outline=1\noutlineThickness=8\noutlineColor=#ff0000\n");
    CHECK(c.outline == 1);
    CHECK(c.outlineThickness == 8);
    CHECK(c.outlineColor == "#ff0000");
    CHECK(ParseConfig("outlineThickness=0\n").outlineThickness == 1);     // clamp low
    CHECK(ParseConfig("outlineThickness=999\n").outlineThickness == 40);  // clamp high
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `build.bat test`
Expected: FAIL to compile (`outline`, `outlineThickness`, `outlineColor` are not members of `Config`).

- [ ] **Step 3: Add fields to `src/config.h`**

Add at the end of the `Config` struct (just before the closing `};`):

```cpp
    // --- Edge outline (zoom indicator) -------------------------------------
    // 1 = draw a solid outline around the screen edges while zoomed (an at-a-glance "you are
    // zoomed" indicator, handy at low zoom); 0 = off (default). Hot-reloadable.
    int         outline          = 0;
    // Outline width in physical pixels (clamped 1-40).
    int         outlineThickness = 4;
    // Outline color as hex RGB ("#rrggbb"; leading '#' optional). Default = Wind accent.
    std::string outlineColor     = "#5b5bd6";
```

- [ ] **Step 4: Parse + clamp in `src/config.cpp`**

In `ParseConfig`, add to the key/value chain (e.g. after the `quickZoomMods` line):

```cpp
            else if (key == "outline")            c.outline = std::stoi(val);
            else if (key == "outlineThickness")   c.outlineThickness = std::stoi(val);
            else if (key == "outlineColor")       c.outlineColor = val;
```

In the clamp block (just before `return c;`), add:

```cpp
    if (c.outlineThickness < 1)  c.outlineThickness = 1;
    if (c.outlineThickness > 40) c.outlineThickness = 40;
```

- [ ] **Step 5: Run tests to verify they pass**

Run: `build.bat test`
Expected: PASS (exit 0).

- [ ] **Step 6: Commit**

```bash
git add src/config.h src/config.cpp tests/test_config.cpp
git commit -m "feat(config): add outline/outlineThickness/outlineColor keys (#92)"
```

---

## Task 3: Default-ini template keys

**Files:**
- Modify: `src/config.cpp` (`LoadConfig`, the written-defaults string)

No unit test (this is the I/O block excluded from the test build). Verified by build + by reading the generated ini.

- [ ] **Step 1: Add documented keys to the template**

In `LoadConfig`, inside the `out << ...` defaults string, append before the final `"onboarded: ..."` lines (or anywhere in the block; put it right after the `cropCapture=0\n` line):

```cpp
               "; outline: 1 = draw a solid outline around the screen edges while zoomed (an\n"
               ";   at-a-glance 'you are zoomed' indicator, handy at low zoom); 0 = off (default)\n"
               "outline=0\n"
               "; outlineThickness: outline width in pixels (1-40)\n"
               "outlineThickness=4\n"
               "; outlineColor: outline color as hex RGB (e.g. #5b5bd6 = Wind accent)\n"
               "outlineColor=#5b5bd6\n"
```

- [ ] **Step 2: Build to verify it compiles**

Run: `build.bat`
Expected: `Wind.exe` builds with no errors.

- [ ] **Step 3: Commit**

```bash
git add src/config.cpp
git commit -m "feat(config): document outline keys in the default ini template (#92)"
```

---

## Task 4: Border shader + RenderFrameParams fields

**Files:**
- Modify: `src/render_shaders.h` (add `kBorderHLSL`)
- Modify: `src/render_engine.h` (`RenderFrameParams`)

No unit test (HLSL + struct); verified by the build in Task 6 and the self-test in Task 7.

- [ ] **Step 1: Add the solid-color shader to `src/render_shaders.h`**

Add just before the closing `}` of `namespace wind {` (after `kCursorHLSL`):

```cpp
// Solid-color quad shader for the zoom edge outline. Same quad expansion as the cursor shader,
// but the PS outputs a constant color (no texture). cb: posClip + sizeClip (clip-space placement)
// + an rgba color. Drawn as a 4-vertex triangle strip, once per screen edge.
inline constexpr const char* kBorderHLSL = R"(
cbuffer CB : register(b0) { float2 posClip; float2 sizeClip; float4 color; };
struct VSOut { float4 pos : SV_POSITION; };
VSOut VSMain(uint id : SV_VertexID) {
    float2 q = float2(id & 1, (id >> 1) & 1);   // (0,0),(1,0),(0,1),(1,1)
    VSOut o;
    o.pos = float4(posClip + q * sizeClip, 0, 1);
    return o;
}
float4 PSMain(VSOut i) : SV_TARGET { return color; }
)";
```

- [ ] **Step 2: Add the fields to `RenderFrameParams` in `src/render_engine.h`**

Add to the `struct RenderFrameParams` (after `cropCapture`):

```cpp
    bool   outline;             // draw the edge outline while zoomed (level > 1.0)
    int    outlineThicknessPx;  // outline width in physical px (clamped in render())
    float  outlineR, outlineG, outlineB;  // outline color 0..1, written straight to the backbuffer
```

- [ ] **Step 3: Commit**

```bash
git add src/render_shaders.h src/render_engine.h
git commit -m "feat(render): add border shader and outline RenderFrameParams fields (#92)"
```

---

## Task 5: Border device resources (build + device-lost recovery)

**Files:**
- Modify: `src/render_engine.cpp` (`State` members, `buildDeviceResources`, `recoverDeviceLost`)

- [ ] **Step 1: Add the resource members to `RenderEngine::State`**

In the `// Cursor pass ...` member group of `struct RenderEngine::State`, add after the cursor pipeline members (e.g. after `ComPtr<ID3D11Buffer> ccb;`):

```cpp
    // Border (edge-outline) pass: a solid-color quad pipeline reused for all four edges.
    ComPtr<ID3D11VertexShader> bvs;
    ComPtr<ID3D11PixelShader> bps;
    ComPtr<ID3D11Buffer> bcb;                  // posClip/sizeClip + rgba for the outline quad
```

- [ ] **Step 2: Build the resources in `buildDeviceResources`**

In `RenderEngine::State::buildDeviceResources()`, after the cursor constant buffer is created (after the `CreateBuffer(&ccbd, ...)` block) and before the blend-state setup, add:

```cpp
    // --- Border (edge-outline) shader pipeline ---
    ID3DBlob* bvsb = CompileShader(kBorderHLSL, "VSMain", "vs_5_0");
    ID3DBlob* bpsb = CompileShader(kBorderHLSL, "PSMain", "ps_5_0");
    if (!bvsb || !bpsb) { RLog("buildDeviceResources: border shader compile failed"); SafeRelease(bvsb); SafeRelease(bpsb); return false; }
    HRESULT hr5 = device->CreateVertexShader(bvsb->GetBufferPointer(), bvsb->GetBufferSize(), nullptr, bvs.ReleaseAndGetAddressOf());
    HRESULT hr6 = device->CreatePixelShader(bpsb->GetBufferPointer(), bpsb->GetBufferSize(), nullptr, bps.ReleaseAndGetAddressOf());
    SafeRelease(bvsb); SafeRelease(bpsb);
    if (FAILED(hr5) || FAILED(hr6)) { RLog("buildDeviceResources: border shader create failed hr5=0x%08lX hr6=0x%08lX", (unsigned long)hr5, (unsigned long)hr6); return false; }

    D3D11_BUFFER_DESC bcbd{};
    bcbd.ByteWidth = 32;   // float2 posClip + float2 sizeClip + float4 color
    bcbd.Usage = D3D11_USAGE_DEFAULT;
    bcbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(device->CreateBuffer(&bcbd, nullptr, bcb.ReleaseAndGetAddressOf()))) { RLog("buildDeviceResources: CreateBuffer(border cb) failed"); return false; }
```

- [ ] **Step 3: Release the resources in `recoverDeviceLost`**

In `RenderEngine::recoverDeviceLost()`, alongside the other shader resets, add (e.g. right after the `s_->cb.Reset(); s_->ccb.Reset();` line):

```cpp
    s_->bvs.Reset(); s_->bps.Reset(); s_->bcb.Reset();
```

(They are rebuilt by the `buildDeviceResources()` call already present later in `recoverDeviceLost`.)

- [ ] **Step 4: Build to verify it compiles**

Run: `build.bat`
Expected: `Wind.exe` builds with no errors.

- [ ] **Step 5: Commit**

```bash
git add src/render_engine.cpp
git commit -m "feat(render): create + recover border shader device resources (#92)"
```

---

## Task 6: Border draw pass

**Files:**
- Modify: `src/render_engine.cpp` (`State::render`)

- [ ] **Step 1: Add the draw pass between the magnify pass and the cursor pass**

In `RenderEngine::State::render(const RenderFrameParams& p)`, insert this block immediately after the `if (haveDesktop) { ... }` magnify-pass block closes and before the `// Cursor pass:` comment:

```cpp
    // Edge-outline pass: four thin solid-color quads at the screen borders, drawn while zoomed.
    // Into the capture-excluded overlay, so it never feeds back into Desktop Duplication. Opaque
    // (no blend) for crisp edges. Gated on level > 1.0 (the overlay is only revealed while zoomed,
    // so this is belt-and-braces). Four trivial draws, only when enabled.
    if (p.outline && p.level > 1.0 && haveDesktop) {
        int t = p.outlineThicknessPx;
        if (t < 1) t = 1;
        const int maxT = (sw < sh ? sw : sh) / 2;   // never let opposite edges overlap/invert
        if (t > maxT) t = maxT;
        if (t > 0) {
            const float r = p.outlineR, g = p.outlineG, b = p.outlineB;
            c->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);   // opaque
            c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
            c->IASetInputLayout(nullptr);
            c->VSSetShader(bvs.Get(), nullptr, 0);
            c->VSSetConstantBuffers(0, 1, bcb.GetAddressOf());
            c->PSSetShader(bps.Get(), nullptr, 0);
            c->PSSetConstantBuffers(0, 1, bcb.GetAddressOf());
            const int edges[4][4] = {              // {x, y, w, h} in physical px
                { 0,      0,      sw,  t          },   // top
                { 0,      sh - t, sw,  t          },   // bottom
                { 0,      t,      t,   sh - 2 * t },   // left
                { sw - t, t,      t,   sh - 2 * t },   // right
            };
            for (int e = 0; e < 4; ++e) {
                const int x = edges[e][0], y = edges[e][1], w = edges[e][2], h = edges[e][3];
                if (w <= 0 || h <= 0) continue;
                const float posClipX  = (float)(x / (double)sw * 2.0 - 1.0);
                const float posClipY  = (float)(1.0 - y / (double)sh * 2.0);
                const float sizeClipX = (float)(w / (double)sw * 2.0);
                const float sizeClipY = (float)(-(h / (double)sh * 2.0));   // clip-y up vs screen-y down
                const float bcbv[8] = { posClipX, posClipY, sizeClipX, sizeClipY, r, g, b, 1.0f };
                c->UpdateSubresource(bcb.Get(), 0, nullptr, bcbv, 0, 0);
                c->Draw(4, 0);
            }
        }
    }
```

- [ ] **Step 2: Build to verify it compiles**

Run: `build.bat`
Expected: `Wind.exe` builds with no errors.

- [ ] **Step 3: Commit**

```bash
git add src/render_engine.cpp
git commit -m "feat(render): draw the zoom edge outline pass (#92)"
```

---

## Task 7: Wire config to params + visual verification

**Files:**
- Modify: `src/main.cpp` (`FillRenderParams`)

- [ ] **Step 1: Populate the new params in `FillRenderParams`**

In `FillRenderParams`, after the `p.cropCapture = (cfg.cropCapture != 0);` line, add:

```cpp
    p.outline = (cfg.outline != 0);
    p.outlineThicknessPx = cfg.outlineThickness;
    float orr = 0.357f, og = 0.357f, ob = 0.839f;   // #5b5bd6 fallback (accent)
    ParseHexColor(cfg.outlineColor, orr, og, ob);
    p.outlineR = orr; p.outlineG = og; p.outlineB = ob;
```

(`config.h` is already included by `main.cpp`, so `ParseHexColor` is in scope.)

- [ ] **Step 2: Build**

Run: `build.bat`
Expected: `Wind.exe` builds with no errors.

- [ ] **Step 3: Visual self-test with the outline enabled**

The overlay is capture-excluded, so only the in-app dump can see it. Enable the outline in the dev ini, then run the self-test (it renders the real path at 4.0x and dumps a PNG):

```bash
echo outline=1>> magnifier.ini
echo outlineColor=#5b5bd6>> magnifier.ini
set WIND_SELFTEST=1
Wind.exe
```

Open `wind_selftest.png`. Expected: a solid 4px indigo (`#5b5bd6`) frame hugging all four screen edges, over the magnified view. Then change `outlineColor` to `#ff0000`, rerun, and confirm a red frame; set `outline=0`, rerun, and confirm no frame.

- [ ] **Step 4: Commit**

```bash
git add src/main.cpp
git commit -m "feat(render): drive the edge outline from config (#92)"
```

---

## Task 8: Config UI - color row type + Display rows

**Files:**
- Modify: `ui/src/lib/Row.svelte` (new `color` row type)
- Modify: `ui/src/settings-schema.js` (three Display rows)

The WebView2 bridge is generic (`getConfig` enumerates every ini key; `setConfig` writes any key), so no host/`ini_edit` changes are needed. `<input type="color">` emits `#rrggbb`, matching the ini default `#5b5bd6`.

- [ ] **Step 1: Add the `color` branch to `ui/src/lib/Row.svelte`**

In the control `{#if ...}` chain (e.g. after the `segmented` branch, before the closing `{/if}`), add:

```svelte
      {:else if row.type === 'color'}
        <input class="color" type="color" {disabled} value={value}
               on:input={e => onChange(e.target.value)} />
```

And add to the `<style>` block:

```css
  .color{width:42px;height:26px;padding:2px;border:1px solid var(--line);border-radius:7px;
         background:transparent;cursor:pointer}
  .color:disabled{opacity:.45;cursor:default}
```

- [ ] **Step 2: Add the rows to the Display section in `ui/src/settings-schema.js`**

In the `{ id:'display', ... rows: [ ... ] }` array, add after the `multiMonitor` row:

```js
    { key:'outline',          type:'toggle', label:'Edge outline',
      desc:'Show an outline around the screen while zoomed.', def:0 },
    { key:'outlineThickness', type:'slider', label:'Outline thickness',
      desc:'Width in pixels.', min:1, max:40, step:1, def:4, dependsOn:'outline' },
    { key:'outlineColor',     type:'color',  label:'Outline color',
      def:'#5b5bd6', dependsOn:'outline' },
```

- [ ] **Step 3: Build the config UI + host**

Run: `build.bat config`
Expected: the Svelte app builds to `ui/dist/` and `WindConfig.exe` compiles with no errors.

- [ ] **Step 4: Manually verify the UI**

Run: `WindConfig.exe`
Expected: in the Display section, an "Edge outline" toggle; turning it on un-greys "Outline thickness" (a 1-40 slider) and "Outline color" (a swatch that opens the OS color picker). Toggle on, pick a color, click Apply; confirm `magnifier.ini` now has `outline=1`, `outlineThickness=...`, `outlineColor=#...`. With Wind.exe running, zoom in and confirm the outline appears in the chosen color and thickness, and updates within ~1s of an Apply (hot-reload).

- [ ] **Step 5: Commit**

```bash
git add ui/src/lib/Row.svelte ui/src/settings-schema.js
git commit -m "feat(ui): expose edge outline settings with a color picker (#92)"
```

---

## Task 9: Final verification + PR

**Files:** none (verification + integration)

- [ ] **Step 1: Run the full unit-test suite**

Run: `build.bat test`
Expected: PASS (exit 0).

- [ ] **Step 2: Build all three binaries**

Run: `build.bat` then `build.bat config`
Expected: `Wind.exe` and `WindConfig.exe` build clean.

- [ ] **Step 3: Clean up the dev ini test edits**

Remove any `outline=1` / test color lines appended to the dev `magnifier.ini` during Task 7 so the working tree is clean (or `git checkout magnifier.ini` if tracked; otherwise delete the appended lines).

- [ ] **Step 4: Push and open the PR**

```bash
git push -u origin feat/zoom-edge-outline
gh pr create --title "Edge outline indicator while zoomed" --body "Closes #92.

Adds an optional solid outline around the screen edges while zoomed - an at-a-glance indicator that zoom is active, most useful at low zoom. Off by default; configurable on/off, thickness (1-40 px), and color (hex) via magnifier.ini and the WindConfig Display section.

- New kBorderHLSL solid-color shader draws four edge quads in the capture-excluded overlay (no feedback loop), gated on level > 1.0.
- New config keys outline/outlineThickness/outlineColor (hot-reloadable); pure ParseHexColor helper with unit tests.
- New native color-picker row type in the config UI.

Spec: docs/superpowers/specs/2026-06-07-zoom-edge-outline-design.md
Verified: build.bat test green; WIND_SELFTEST dump shows the frame; manual hot-reload check via WindConfig."
```

---

## Self-Review notes

- **Spec coverage:** behavior/always-on gate (Tasks 6/7), 4px accent default (Tasks 2/3/4), physical-px thickness + clamp (Tasks 2/6), shader + draw pass (Tasks 4/5/6), config keys + hex parse + wiring (Tasks 1/2/3/7), config UI color picker + Display rows (Task 8), unit tests + visual verification (Tasks 1/2/7/9). All covered.
- **Type consistency:** `ParseHexColor(const std::string&, float&, float&, float&)` is declared, implemented, tested, and called identically. `RenderFrameParams` fields `outline`/`outlineThicknessPx`/`outlineR/G/B` are defined in Task 4 and consumed in Tasks 6/7 with the same names. `Config` fields `outline`/`outlineThickness`/`outlineColor` match across Tasks 2/3/7 and the schema keys in Task 8. Constant-buffer size 32 bytes matches the 8-float `bcbv` upload.
- **Default color string** is `#5b5bd6` (with leading `#`) everywhere - struct default, ini template, UI def, and the `<input type="color">` value format - so the native picker is happy and `ParseHexColor` tolerates the `#`.
