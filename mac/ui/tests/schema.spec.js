// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
// The Mac settings schema (src/settings-schema.js): pages, cards, row shapes, copy rules, the
// keybind contract and the gates. The Config side (keys, defaults, ranges) is
// config-defaults.nodetest.mjs.
import { test, expect } from '@playwright/test';
import { groups, groupRows, allRows, rowKeys, schemaDefaults, missingPermissions } from '../src/settings-schema.js';

const TYPES = ['keybind', 'slider', 'toggle', 'select', 'palette', 'button', 'macstatus', 'about'];

test('five pages in order, the last two below the divider', () => {
  expect(groups.map((g) => g.id)).toEqual(['hotkeys', 'zoom', 'view', 'prefs', 'about']);
  expect(groups.map((g) => g.label)).toEqual(['Hotkeys', 'Zoom', 'View', 'Preferences', 'About']);
  expect(groups.filter((g) => g.bottom).map((g) => g.id)).toEqual(['prefs', 'about']);
  for (const g of groups) expect(g.icon, g.id).toBeTruthy();
});

test('every row has a known type and a unique key; labelled rows have a description', () => {
  const keys = allRows.map((r) => r.key);
  expect(new Set(keys).size).toBe(keys.length);
  for (const r of allRows) {
    expect(TYPES, r.key).toContain(r.type);
    if (r.label) expect(r.desc, r.key).toBeTruthy();
  }
});

test('a captioned card has two or more rows', () => {
  for (const g of groups) for (const c of g.cards) if (c.caption) expect(c.rows.length, g.id + ' ' + c.caption).toBeGreaterThanOrEqual(2);
});

test('the advanced rows are the Easing card and the fine steps threshold', () => {
  expect(allRows.filter((r) => r.adv).map((r) => r.key).sort())
    .toEqual(['precisionBelow', 'smoothZoom', 'smoothZoomAccel', 'smoothZoomRamp'].sort());
});

test('copy: no em-dash, no "Enable", no trailing period, no description restating its label', () => {
  for (const r of allRows) {
    for (const t of [r.label, r.desc, r.tag, r.btn, ...(r.optionLabels ? Object.values(r.optionLabels) : [])].filter(Boolean)) {
      expect(t, r.key).not.toContain('—');
      expect(t, r.key).not.toMatch(/\.$/);
    }
    if (r.type === 'toggle') expect(r.label, r.key).not.toMatch(/^Enable/);
    if (r.label && r.desc) expect(r.desc.toLowerCase(), r.key).not.toBe(r.label.toLowerCase());
  }
  for (const g of groups) if (g.desc) expect(g.desc).not.toContain('—');
});

test('keybind rows follow the builders\' contract', () => {
  const binds = allRows.filter((r) => r.type === 'keybind');
  expect(binds.map((r) => r.key)).toEqual(['zoomIn', 'zoomOut', 'wheelZoomMods', 'quickZoom', '__pan', 'hideCursor', 'inspect', 'quit']);
  for (const r of binds) {
    if (r.panArrows) expect(r.panKeys).toEqual(['panLeft', 'panUp', 'panRight', 'panDown']);
    else if (r.wheelMods) expect(r.max).toBeUndefined();
    else expect(r.max, r.key).toBe(r.key === 'zoomIn' || r.key === 'zoomOut' ? 3 : 1);   // only zoom takes several
    expect(r.keywords.length, r.key).toBeGreaterThanOrEqual(3);
  }
  expect(allRows.find((r) => r.key === 'zoomIn').max).toBe(3);
  expect(allRows.find((r) => r.key === 'zoomOut').max).toBe(3);
  expect(rowKeys(allRows.find((r) => r.key === '__pan'))).toEqual(['panLeft', 'panUp', 'panRight', 'panDown']);
  expect(rowKeys(allRows.find((r) => r.key === '__about'))).toEqual([]);
});

test('gates point at real rows and values', () => {
  const byKey = Object.fromEntries(allRows.map((r) => [r.key, r]));
  for (const r of allRows) {
    for (const gate of [r.showIf, r.offIf].filter(Boolean)) {
      expect(byKey[gate.key], r.key + ' gate ' + gate.key).toBeTruthy();
      const v = gate.eq !== undefined ? gate.eq : gate.ne;
      const opts = byKey[gate.key].options || (byKey[gate.key].type === 'toggle' ? ['0', '1'] : null);
      if (opts) expect(opts, r.key).toContain(String(v));
    }
  }
});

test('the macOS zoom rows show only on the macOS zoom engine and sit on View', () => {
  const view = groups.find((g) => g.id === 'view');
  const card = view.cards.find((c) => c.caption === 'Transform engine');
  expect(card.rows.map((r) => r.key)).toEqual(['slowPrecision', 'precisionBelow', 'slowPanGlide', 'fullPointerSpeed', 'cursor']);
  for (const r of card.rows) expect(r.showIf, r.key).toEqual({ key: 'engine', eq: 'zoom' });
  // Nothing else depends on the engine but each engine's own Scaling row (Engine card on Zoom).
  const scaling = { smoothImages: { key: 'engine', eq: 'zoom' }, renderScaling: { key: 'engine', eq: 'draw' } };
  for (const r of allRows.filter((x) => !card.rows.includes(x))) {
    if (scaling[r.key]) expect(r.showIf, r.key).toEqual(scaling[r.key]);
    else expect(r.showIf?.key, r.key).not.toBe('engine');
  }
});

test('Scaling: Render offers Nearest and xBR (Windows\' 0 and 1); Transform macOS\'s two', () => {
  const zoom = groups.find((g) => g.id === 'zoom');
  const r = groupRows(zoom).find((x) => x.key === 'renderScaling');
  expect(r.options).toEqual(['0', '1']);                        // mean what they mean on Windows
  expect(r.optionLabels).toEqual({ '0': 'Nearest', '1': 'xBR' });
  expect(r.def).toBe('0');
  const t = groupRows(zoom).find((x) => x.key === 'smoothImages');
  expect(t.options).toEqual(['0', '1']);
});

test('the engine select: Render or Transform (default), in the Engine card on Zoom', () => {
  const zoom = groups.find((g) => g.id === 'zoom');
  const e = groupRows(zoom).find((r) => r.key === 'engine');
  expect(e.options).toEqual(['draw', 'zoom']);
  expect(e.optionLabels).toEqual({ draw: 'Render', zoom: 'Transform' });
  expect(e.def).toBe('zoom');
  expect(schemaDefaults().engine).toBe('zoom');
});

test('the macOS card: status rows, the login switch, log detail and four buttons', () => {
  const prefs = groups.find((g) => g.id === 'prefs');
  const mac = prefs.cards.find((c) => c.caption === 'macOS');
  expect(mac.rows.map((r) => [r.key, r.type])).toEqual([
    ['__accessibility', 'macstatus'], ['__screenRecording', 'macstatus'], ['__zoomShortcuts', 'macstatus'],
    ['__loginItem', 'macstatus'], ['logLevel', 'select'],
    ['__showLog', 'button'], ['__openIni', 'button'], ['__diagnostics', 'button'], ['__rerunSetup', 'button'],
  ]);
  const act = Object.fromEntries(mac.rows.map((r) => [r.key, r.action]));
  expect(act).toMatchObject({ __accessibility: 'openAccessibility', __screenRecording: 'openScreenRecording',
    __zoomShortcuts: 'fixZoomShortcuts', __loginItem: 'setLogin', __showLog: 'showLog', __openIni: 'openIni',
    __diagnostics: 'exportDiagnostics', __rerunSetup: 'rerunSetup' });
});

test('missingPermissions: Accessibility always, Screen Recording on the render engine only', () => {
  expect(missingPermissions(null, 'draw')).toEqual([]);
  expect(missingPermissions({ accessibility: true, screenRecording: true }, 'draw')).toEqual([]);
  expect(missingPermissions({ accessibility: false, screenRecording: false }, 'draw')).toEqual(['accessibility', 'screenRecording']);
  expect(missingPermissions({ accessibility: false, screenRecording: false }, 'zoom')).toEqual(['accessibility']);
  expect(missingPermissions({ accessibility: true }, 'draw')).toEqual([]);   // unknown is not missing
});

test('no Windows-only keys survive', () => {
  const keys = allRows.flatMap((r) => [r.key, ...rowKeys(r)]);
  for (const k of ['model', 'uiTheme', 'trayPerf', 'trayPinned', 'noSwallowApps', 'lockApps', 'renderExclude',
    'colorWarmPct', 'colorDimPct', '__profiles', 'diagnostics', 'zoomInVk', 'zoomInButton', 'cursorLockVk', 'lockTracking', 'panMaxLagPt'])
    expect(keys, k).not.toContain(k);
});
