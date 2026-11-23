// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
// The menu bar panel's view model (pure, no DOM; node tests load it): what the panel shows from the
// config values (strings, as SettingsWeb.values sends them), the toggle and engine writes, and the
// Performance header's text and sparkline. Ports of Windows' flyout_model.h BuildView, ToggleOn,
// ToggleChanges, BuildPerf and SparkNorm, and tray_status.h FormatZoom. Slider ranges, steps and units
// come from the Settings rows (settings-schema.js), so the panel and Settings never disagree.
import { parseTray, SLIDERS, TOGGLES } from '../menubar/menuModel.js';
import { allRows } from '../settings-schema.js';

// The engine picker, Windows' order (Transform first) with the Mac's third engine. Values are config.json's.
export const ENGINES = [
  { value: 'zoom', label: 'Transform' },
  { value: 'draw', label: 'Render' },
  { value: 'backdrop', label: 'Backdrop' },
];
export const engineLabel = (v) => (ENGINES.find((e) => e.value === v) || ENGINES[0]).label;

const rowOf = (key) => allRows.find((r) => r.key === key);

// The slider a key stands for: its range and step from Settings.
export function sliderSpec(key) {
  const r = rowOf(key);
  const item = SLIDERS.find((s) => s.key === key);
  if (!r || !item) return null;
  return { key, icon: item.icon, name: item.name, min: r.min, max: r.max, step: r.step, def: r.def, unit: r.unit || '' };
}

export function sliderValue(spec, values) {
  const n = Number(values[spec.key]);
  const v = values[spec.key] === undefined || values[spec.key] === '' || !Number.isFinite(n) ? spec.def : n;
  return Math.min(spec.max, Math.max(spec.min, v));
}

// "Keep cursor centred" is one segment for two settings: on only when both mouseAlign and trackAlign are
// 0 (centred), so a mixed state reads off and a click sets both (flyout_model.h ToggleOn).
export function toggleOn(key, values) {
  const n = (k, d) => (values[k] === undefined || values[k] === '' ? d : Number(values[k]));
  if (key === 'keepEdges') return n('mouseAlign', 0) === 0 && n('trackAlign', 0) === 0;
  if (key === 'trackCaret') return n('trackCaret', 1) !== 0;
  if (key === 'trackFocus') return n('trackFocus', 0) !== 0;
  return false;
}

// The config writes one toggle click makes: { key: value }.
export function toggleChanges(key, turnOn) {
  if (key === 'keepEdges') { const a = turnOn ? '0' : '1'; return { mouseAlign: a, trackAlign: a }; }
  return { [key]: turnOn ? '1' : '0' };
}

// What the panel shows, in Settings > Menu bar's order: { perf, sliders: [spec + value], toggles:
// [{key, icon, name, on}], engine: bool }. The engine item is the picker under the group, never a segment.
export function buildView(values) {
  const t = parseTray(values);
  const sliders = t.sliders.filter((i) => i.on).map((i) => sliderSpec(i.key)).filter(Boolean)
    .map((s) => ({ ...s, value: sliderValue(s, values) }));
  let engine = false;
  const toggles = [];
  for (const i of t.toggles) {
    if (!i.on) continue;
    if (i.key === 'engine') { engine = true; continue; }
    const meta = TOGGLES.find((x) => x.key === i.key);
    toggles.push({ key: i.key, icon: meta.icon, name: meta.name, on: toggleOn(i.key, values) });
  }
  return { perf: t.perf, sliders, toggles, engine };
}

// "7.4x" (one decimal, 7.98 -> "8.0x") or "Idle" at 1x (tray_status.h FormatZoom).
export function formatZoom(level) {
  const lv = Number(level);
  if (!(lv > 1.001)) return 'Idle';
  let whole = Math.floor(lv), tenth = Math.round((lv - whole) * 10);
  if (tenth >= 10) { tenth = 0; whole += 1; }
  return `${whole}.${tenth}x`;
}

const median = (v) => { const s = [...v].sort((a, b) => a - b); return s[Math.floor(s.length / 2)]; };
const mean = (v) => v.reduce((a, b) => a + b, 0) / v.length;

// The frame-pacing trace, 0 (bottom) .. 1 (top) on a scale of twice the median, so a healthy trace is a
// flat line at 0.5 and a stall spikes above it. Anything under 1.5x the median reads as the median (wake
// jitter is not a missed frame). At most `maxPts` points, each the maximum of its bucket, so a stall is
// never averaged away. Fewer than 8 samples: [] (the header draws a dotted rule).
export function sparkNorm(ms, maxPts = 64) {
  if (!ms || ms.length < 8 || maxPts < 2) return [];
  const n = ms.length, med = median(ms), top = med > 0.01 ? med * 2 : 20, m = Math.min(n, maxPts), out = [];
  for (let i = 0; i < m; i++) {
    const a = Math.floor((i * n) / m), b = Math.max(a + 1, Math.floor(((i + 1) * n) / m));
    let v = 0;
    for (let j = a; j < b && j < n; j++) v = Math.max(v, Math.min((ms[j] < med * 1.5 ? med : ms[j]) / top, 1));
    out.push(v);
  }
  return out;
}

// The Performance header: { zoomed, zoom, haveFps, fps, frameMs, spark } (flyout_model.h BuildPerf). The
// fps figure comes from the mean, not the median (wake jitter pairs cancel in a mean; tick_stats.h).
export function perfView(level, ms) {
  const zoom = formatZoom(level);
  const p = { zoomed: zoom !== 'Idle', zoom, haveFps: false, fps: 0, frameMs: '– ms', spark: [] };
  if (ms && ms.length >= 8) {
    const m = mean(ms);
    p.haveFps = true;
    p.fps = Math.round(m > 0.0001 ? 1000 / m : 0);
    p.frameMs = `${m.toFixed(1)} ms`;
    p.spark = sparkNorm(ms, 64);
  }
  return p;
}

// SVG polyline points for a spark trace in a w x h box (1 px inset top and bottom, like the flyout).
export function sparkPoints(spark, w, h) {
  if (spark.length < 2) return '';
  return spark.map((v, i) => `${((w * i) / (spark.length - 1)).toFixed(1)},${(h - 1 - v * (h - 2)).toFixed(1)}`).join(' ');
}

// The Mac-only banners the panel shows, only when needed: Accessibility missing, macOS Zoom shortcuts on.
export function banners(mac) {
  const out = [];
  if (mac && mac.accessibility === false) out.push('accessibility');
  if (mac && Array.isArray(mac.zoomShortcuts) && mac.zoomShortcuts.length) out.push('zoomShortcuts');
  return out;
}
