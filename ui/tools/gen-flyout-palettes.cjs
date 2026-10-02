// Generates src/tray_app/flyout_palettes.h (the tray flyout's 4 themes x 2 modes) from the same mockup palettes
// as ui/tools/gen-themes.cjs, evaluating the tokens tray08.css derives (all color-mix(in srgb) done here):
//   toff = fg 12% over card, toffh = fg 18% over card, ton = segon || fill 36% over toff, tonh = segon || fill 44%,
//   tonic = segonfg || accent 70% over fg, scrim = bg at 62%, segline = card (the 1px gap shows the group's card).
//
//   node tools/gen-flyout-palettes.cjs [mockup-ia-dir]
//
// Wind grey keeps the values the flyout already drew (docs/design/tray-2026-10/j01), like Settings keeps today's tokens.
const fs = require('fs');
const path = require('path');
const iaDir = process.argv[2] || 'C:/Users/Admin/Documents/Claude/wind-settings-mockups/ia';
const all = Object.assign({}, require(path.join(iaDir, 'palettes08.cjs')),
  require(path.join(iaDir, 'palettes-b1.cjs')), require(path.join(iaDir, 'palettes-b2.cjs')));
const ORDER = [['grey', 'grey'], ['ember', 'ember'], ['ocean', 'b2_ocean'], ['hicon', 'b1_hicon']];

const hex = (h) => { h = h.replace('#', ''); if (h.length === 3) h = h.split('').map((c) => c + c).join(''); return parseInt(h, 16); };
const rgbOf = (n) => [(n >> 16) & 255, (n >> 8) & 255, n & 255];
const mix = (a, b, t) => { // t of a over b
  const A = rgbOf(a), B = rgbOf(b);
  return (A.map((v, i) => Math.round(v * t + B[i] * (1 - t))).reduce((s, v) => (s << 8) | v, 0)) >>> 0;
};
const px = (v) => parseFloat(v);
const H = (n) => '0x' + n.toString(16).padStart(6, '0');
const f = (v) => { const s = String(+(+v).toFixed(3)); return (s.includes('.') ? s : s + '.') + 'f'; };

// today's flyout look (MakeTheme in flyout_draw.cpp before #318), the Wind grey rows
const GREY = {
  dark: { menu: 0x000000, card: 0x121212, menub: 0x333333, fg: 0xf2f2f2, fg2: 0xd0d0d0, fg3: 0xb4b6ba, rule: 0x303236, hl: 0x2d2d2d,
    glyph: 0xb0b0b0, spark: 0x2fbfa5, fill: 0x2fbfa5, fillline: 0, filllineA: 0, lift: 0x0b0b0b, scrim: 0x000000, scrimA: .55,
    band: 0x0a0a0a, track: 0x3d3d3d, off: 0x303033, offh: 0x3b3b3f, offic: 0xc8cad0, onic: 0xa9ece0, on: 0x1f5650, onh: 0x266560,
    onb: 0x2fbfa5, onbA: .4, segline: 0x000000, focus: 0xf2f2f2, aurora: .62, tint: 0, tintA: 0, invert: 0, legacyLight: 0 },
  light: { menu: 0xffffff, card: 0xffffff, menub: 0xd9d9d9, fg: 0x0a0a0a, fg2: 0x2e2e2e, fg3: 0x45484d, rule: 0xc6c9ce, hl: 0xececec,
    glyph: 0x555555, spark: 0x087a67, fill: 0x087a67, fillline: 0, filllineA: 0, lift: 0xf6f7f8, scrim: 0xffffff, scrimA: .78,
    band: 0xf1f2f4, track: 0xd0d3d6, off: 0xe5e6e8, offh: 0xdadbde, offic: 0x3d4147, onic: 0x0a5a4d, on: 0xbfe2db, onh: 0xb2d9d1,
    onb: 0x087a67, onbA: .4, segline: 0xffffff, focus: 0x0a0a0a, aurora: .85, tint: 0, tintA: 0, invert: 0, legacyLight: 1 },
};
// Settings control greys (ui/src/design/themes.css, Wind grey): the toggle bar and dropdown match them.
GREY.dark.off = 0x0e0e0e; GREY.dark.ring = 0x353535; GREY.dark.segline = 0x353535;
GREY.light.off = 0xf7f7f7; GREY.light.ring = 0xd8d8d8; GREY.light.segline = 0xd8d8d8;
for (const m of [GREY.dark, GREY.light]) m.offh = mixN(m.fg, m.off, .06);
function mixN(a, b, t) {   // a over b by t, 0xRRGGBB numbers
  const ch = (s) => [(s >> 16) & 255, (s >> 8) & 255, s & 255];
  const x = ch(a), y = ch(b);
  return x.map((v, i) => Math.round(v * t + y[i] * (1 - t))).reduce((acc, v) => (acc << 8) | v, 0);
}
function derive(k) {
  const card = hex(k.card), fg = hex(k.fg), fill = hex(k.fill), accent = hex(k.pbg);
  const toff = mix(fg, card, .12);   // base for the ON tint only (unchanged look)
  const chip = hex(k.chip), chipb = hex(k.chipb), chiph = mix(fg, chip, .06);
  const ton = k.segon ? hex(k.segon) : mix(fill, toff, .36), tonh = k.segon ? hex(k.segon) : mix(fill, toff, .44);
  const tonic = k.segonfg ? hex(k.segonfg) : mix(accent, fg, .7);
  const fl = k.fillline && k.fillline !== 'transparent';
  return { menu: hex(k.bg), card, menub: hex(k.chipb), fg, fg2: hex(k.fg2), fg3: hex(k.fg3), rule: hex(k.line2), hl: hex(k.hover),
    glyph: hex(k.glyph), spark: accent, fill, fillline: fl ? hex(k.fillline) : 0, filllineA: fl ? 1 : 0, lift: card,
    scrim: hex(k.bg), scrimA: .62, band: hex(k.bnbg), track: hex(k.track), off: chip, offh: chiph, offic: hex(k.glyph), onic: tonic,
    on: ton, onh: tonh, onb: fill, onbA: .4, segline: chipb, ring: chipb, focus: hex(k.focus), aurora: Math.min(1, +k.bnop * 3.4),
    tint: hex(k.bntint), tintA: +k.bntop, invert: k.bnfilter && k.bnfilter.startsWith('invert') ? 1 : 0, legacyLight: 0 };
}
const NAMES = ['menu', 'card', 'menub', 'fg', 'fg2', 'fg3', 'rule', 'hl', 'glyph', 'spark', 'fill', 'fillline', 'lift', 'scrim', 'band',
  'track', 'off', 'offh', 'offic', 'onic', 'on', 'onh', 'onb', 'segline', 'focus', 'tint', 'ring'];
function mode(m) {
  return '{ ' + NAMES.map((n) => H(m[n])).join(', ') + ',\n      '
    + [f(m.filllineA), f(m.scrimA), f(m.onbA), f(m.aurora), f(m.tintA), m.invert, m.legacyLight].join(', ') + ' }';
}
let out = `#pragma once
// GENERATED by ui/tools/gen-flyout-palettes.cjs from the mockup palettes (palettes08/-b1/-b2.cjs + tray08.css). Do not edit by hand.
// The tray flyout's 4 themes x 2 modes, the same ids and values as the Settings themes (ui/src/design/themes.css), plus the
// tray tokens tray08.css derives (toggle off/on fills, on icon, on-segment overrides).
// Pure data: no windows.h. Colours are 0xRRGGBB; the float fields are alphas and flags.
#include <string>

namespace wind { namespace Flyout {

struct PaletteMode {
    unsigned menu, card, menub, fg, fg2, fg3, rule, hl, glyph, spark, fill, fillline, lift, scrim, band, track,
             off, offh, offic, onic, on, onh, onb, segline, focus, tint,
             ring;   // the 1 px outline of the toggle bar and the dropdown (Settings --chipb)
    float filllineA;    // 1 = a 1 px ring around the slider fill (High contrast), 0 = none
    float scrimA;       // the header scrim, over menu
    float onbA;         // alpha of the open dropdown ring (onb)
    float aurora;       // header image opacity
    float tintA;        // colour-blend tint over the header image (0 = none)
    int invert;         // the light header image is inverted
    int legacyLight;    // Wind grey light: the full invert/hue-rotate/saturate/contrast chain the j01 reference uses
};

struct FlyoutPalette {
    const char* id;
    int rc, rk, rs;     // window, control and small radii in DIPs (sharp for hicon)
    PaletteMode dark, light;
};

inline constexpr int kPaletteCount = ${ORDER.length};
inline const FlyoutPalette kPalettes[kPaletteCount] = {
`;
for (const [id, mid] of ORDER) {
  const p = all[mid];
  const d = id === 'grey' ? GREY.dark : derive(p.dark), l = id === 'grey' ? GREY.light : derive(p.light);
  out += `    { "${id}", ${px(p.radii.rc)}, ${px(p.radii.rk)}, ${px(p.radii.rs)},\n      ${mode(d)},\n      ${mode(l)} },\n`;
}
out += `};

// The palette index for a uiPalette value; anything unknown or empty is Wind grey (index 0).
inline int PaletteIndex(const std::string& id) {
    for (int i = 0; i < kPaletteCount; ++i) if (id == kPalettes[i].id) return i;
    return 0;
}
inline const FlyoutPalette& FindPalette(const std::string& id) { return kPalettes[PaletteIndex(id)]; }
inline const PaletteMode& PaletteFor(int index, bool dark) {
    const FlyoutPalette& p = kPalettes[index >= 0 && index < kPaletteCount ? index : 0];
    return dark ? p.dark : p.light;
}
inline const FlyoutPalette& PaletteAt(int index) { return kPalettes[index >= 0 && index < kPaletteCount ? index : 0]; }
// The list popup is 2 DIPs tighter than the window when the window is soft (10 -> 8), the same radius when sharp.
inline float ListRadiusFor(const FlyoutPalette& p) { return p.rc >= 10 ? (float)(p.rc - 2) : (float)p.rc; }

}}  // namespace wind::Flyout
`;
fs.writeFileSync(path.join(__dirname, '../../src/tray_app/flyout_palettes.h'), out);
console.log('flyout_palettes.h', out.length, 'bytes');
