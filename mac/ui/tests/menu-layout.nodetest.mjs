// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import { parseList, parseTray, serialiseList, SLIDERS, TOGGLES, MAX_SLIDERS } from '../src/menubar/menuModel.js';

// Plain node:test, no browser: run with `npm run test:rules`.
// The menu bar lists are read by the settings page (menuModel.js, ui-shared/src/tray/listModel.js) and by
// the panel (Sources/WindCore/MenuLayout.swift). Both run Tests/fixtures/menu_layout_cases.json.

const here = dirname(fileURLToPath(import.meta.url));
const read = (...p) => readFileSync(join(here, '..', '..', ...p), 'utf8');
const cases = JSON.parse(read('Tests', 'fixtures', 'menu_layout_cases.json')).cases;
const core = JSON.parse(read('Tests', 'fixtures', 'config_defaults.json')).keys;
const KEY = { sliders: ['menuSliders', 'menuSliderOrder'], toggles: ['menuToggles', 'menuToggleOrder'] };

test('the shared parse cases', () => {
  assert.ok(cases.length >= 10);
  for (const c of cases) {
    const [en, od] = KEY[c.kind];
    const v = {};
    if (c.enabled !== null) v[en] = c.enabled;
    if (c.order !== null) v[od] = c.order;
    const items = parseList(v, c.kind);
    assert.equal(items.map((i) => i.key).join(','), c.wantOrder, c.name + ' (order)');
    assert.equal(items.filter((i) => i.on).map((i) => i.key).join(','), c.wantOn, c.name + ' (on)');
  }
});

test('the items match MenuLayout.swift, and the defaults match Config()', () => {
  const swift = read('Sources', 'WindCore', 'MenuLayout.swift');
  const list = (name) => JSON.parse('[' + swift.match(new RegExp('static let ' + name + ' = \\[([^\\]]*)\\]'))[1] + ']');
  assert.deepEqual(SLIDERS.map((i) => i.key), list('sliders'));
  assert.deepEqual(TOGGLES.map((i) => i.key), list('toggles'));
  assert.equal(MAX_SLIDERS, Number(swift.match(/maxSliders = (\d+)/)[1]));
  const t = parseTray({});
  assert.equal(t.perf, core.menuPerf.def === '1');
  assert.equal(serialiseList('sliders', t.sliders).menuSliders, core.menuSliders.def);
  assert.equal(serialiseList('toggles', t.toggles).menuToggles, core.menuToggles.def);
  // An empty order is the default order, which is what Config ships.
  assert.equal(core.menuSliderOrder.def, '');
  assert.equal(core.menuToggleOrder.def, '');
  assert.deepEqual(parseList({ menuSliderOrder: '' }, 'sliders'), t.sliders);
});

test('every slider is a real slider row, every toggle but keepEdges a real row', async () => {
  const { allRows } = await import('../src/settings-schema.js');
  const sliders = new Set(allRows.filter((r) => r.type === 'slider').map((r) => r.key));
  for (const i of SLIDERS) assert.ok(sliders.has(i.key), i.key);
  const keys = new Set(allRows.map((r) => r.key));
  for (const i of TOGGLES) if (i.key !== 'keepEdges') assert.ok(keys.has(i.key), i.key);
  assert.ok(keys.has('mouseAlign') && keys.has('trackAlign'));
});
