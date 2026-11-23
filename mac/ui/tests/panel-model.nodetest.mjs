// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
// The menu bar panel's view model (src/panel/panelModel.js): the ports of Windows' flyout rules.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { buildView, formatZoom, sparkNorm, perfView, toggleOn, toggleChanges, sliderSpec, banners, engineLabel } from '../src/panel/panelModel.js';
import { SLIDERS } from '../src/menubar/menuModel.js';

test('formatZoom: one decimal, rounding up a whole, Idle at 1x', () => {
  assert.equal(formatZoom(1), 'Idle');
  assert.equal(formatZoom(1.0005), 'Idle');
  assert.equal(formatZoom(7.42), '7.4x');
  assert.equal(formatZoom(7.98), '8.0x');
  assert.equal(formatZoom(12.05), '12.1x');
});

test('every slider has a Settings row with a range', () => {
  for (const s of SLIDERS) {
    const spec = sliderSpec(s.key);
    assert.ok(spec, s.key);
    assert.ok(spec.max > spec.min && spec.step > 0, s.key);
  }
});

test('buildView: defaults, values clamped, engine apart from the group', () => {
  const v = buildView({ warmthPct: '250', dimPct: 'x' });
  assert.equal(v.perf, true);
  assert.deepEqual(v.sliders.map((s) => [s.key, s.value]), [['warmthPct', 100], ['dimPct', 100]]);
  assert.deepEqual(v.toggles.map((t) => t.key), ['trackCaret', 'trackFocus', 'keepEdges']);
  assert.equal(v.engine, true);
  assert.equal(buildView({ menuToggles: 'trackFocus' }).engine, false);
});

test('Keep cursor centred: on only when both alignments are centred; a click writes both', () => {
  assert.equal(toggleOn('keepEdges', {}), true);
  assert.equal(toggleOn('keepEdges', { mouseAlign: '0', trackAlign: '1' }), false);
  assert.deepEqual(toggleChanges('keepEdges', false), { mouseAlign: '1', trackAlign: '1' });
  assert.deepEqual(toggleChanges('keepEdges', true), { mouseAlign: '0', trackAlign: '0' });
  assert.deepEqual(toggleChanges('trackCaret', false), { trackCaret: '0' });
  assert.equal(toggleOn('trackCaret', {}), true);
  assert.equal(toggleOn('trackFocus', {}), false);
});

test('sparkNorm: flat at 0.5, jitter flattened, a stall kept at full height', () => {
  assert.deepEqual(sparkNorm([1, 2, 3]), []);
  const flat = sparkNorm(Array(256).fill(7));
  assert.equal(flat.length, 64);
  assert.ok(flat.every((v) => v === 0.5));
  const jitter = sparkNorm([...Array(100).fill(7), 9, ...Array(100).fill(7)]);
  assert.ok(jitter.every((v) => v === 0.5));
  const stall = sparkNorm([...Array(200).fill(7), 50, ...Array(55).fill(7)]);
  assert.equal(Math.max(...stall), 1);
  assert.equal(stall.filter((v) => v === 1).length, 1);
});

test('perfView: fps from the mean, the mean in ms', () => {
  const p = perfView(3, Array(100).fill(8));
  assert.deepEqual([p.zoom, p.haveFps, p.fps, p.frameMs], ['3.0x', true, 125, '8.0 ms']);
  const idle = perfView(1, []);
  assert.deepEqual([idle.zoom, idle.zoomed, idle.haveFps, idle.frameMs], ['Idle', false, false, '– ms']);
});

test('banners and engine labels', () => {
  assert.deepEqual(banners({ accessibility: true, zoomShortcuts: [] }), []);
  assert.deepEqual(banners({ accessibility: false, zoomShortcuts: ['a'] }), ['accessibility', 'zoomShortcuts']);
  assert.deepEqual(banners({}), []);
  assert.equal(engineLabel('draw'), 'Render');
  assert.equal(engineLabel('backdrop'), 'Backdrop');
  assert.equal(engineLabel('bogus'), 'Transform');
});
