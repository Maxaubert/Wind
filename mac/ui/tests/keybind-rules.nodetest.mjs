// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import { checkBindText, parseBind, formatBind, droppedBinds } from '../src/lib/keybindRules.js';
import { bindLabel, bindSpoken, CODE_TO_KEY, KEY_NAMES } from '../src/lib/bindNames.js';
import { panState, panPatch, placePatch, removePatch, unitsOf } from '../src/lib/bindings.js';

// Plain node:test, no browser: run with `npm run test:rules`.
// The UI mirror must agree with Binds.swift on every shared case (wind-checks reads the same file).
test('UI keybind rules match the shared Mac case list', () => {
  const here = dirname(fileURLToPath(import.meta.url));
  const lines = readFileSync(join(here, '..', '..', 'Tests', 'fixtures', 'mac_keybind_cases.txt'), 'utf8').split(/\r?\n/);
  let n = 0;
  for (const raw of lines) {
    const line = raw.trim();
    if (!line || line.startsWith('#')) continue;
    const sp = line.lastIndexOf(' ');
    const bind = line.slice(0, sp).trim(), want = line.slice(sp + 1);
    assert.equal(checkBindText(bind), want, line);
    n++;
  }
  assert.ok(n > 100, `only ${n} cases read`);
});

test('formatBind writes the canonical text', () => {
  assert.equal(formatBind(parseBind('opt+ctrl+left')), 'ctrl+opt+left');
  assert.equal(formatBind(parseBind('cmd+esc')), 'cmd+escape');
  assert.equal(formatBind(parseBind('ctrl++')), 'ctrl+equal');
  assert.equal(formatBind(parseBind('Shift+Mouse3')), 'shift+mouse3');
  assert.equal(formatBind(parseBind('alt+control')), 'ctrl+opt');
});

test('every DOM code maps to a key Binds.swift reads', () => {
  for (const [code, name] of Object.entries(CODE_TO_KEY)) assert.ok(KEY_NAMES.has(name), code + ' -> ' + name);
});

test('Mac display names', () => {
  assert.equal(bindLabel(parseBind('ctrl+opt+equal')), '⌃⌥=');
  assert.equal(bindLabel(parseBind('mouse4')), 'Mouse 4');
  assert.equal(bindLabel(parseBind('shift+mouse3')), '⇧ Mouse 3');
  assert.equal(bindLabel(parseBind('f18')), 'F18');
  assert.equal(bindLabel(parseBind('cmd+shift+opt+ctrl+left')), '⌃⌥⇧⌘←');
  assert.equal(bindSpoken(parseBind('ctrl+opt+equal')), 'Control Option Equals');
});

test('bind list patches', () => {
  const row = { key: 'zoomIn', max: 2 };
  const values = { zoomIn: 'mouse4,ctrl+opt+equal' };
  assert.deepEqual(unitsOf(row, values).map((u) => u.text), ['mouse4', 'ctrl+opt+equal']);
  assert.deepEqual(removePatch(row, values, 0), { zoomIn: 'ctrl+opt+equal' });
  assert.deepEqual(placePatch(row, values, 1, 'f13'), { zoomIn: 'mouse4,f13' });
  assert.deepEqual(placePatch(row, { zoomIn: '' }, null, 'mouse2'), { zoomIn: 'mouse2' });
  assert.deepEqual(placePatch(row, values, 1, 'mouse4'), { zoomIn: 'mouse4' });   // no duplicates
});

test('pan state and patch', () => {
  const row = { key: '__pan', panArrows: true, panKeys: ['panLeft', 'panUp', 'panRight', 'panDown'] };
  const p = panPatch(row, ['ctrl', 'opt']);
  assert.deepEqual(p, { panLeft: 'ctrl+opt+left', panUp: 'ctrl+opt+up', panRight: 'ctrl+opt+right', panDown: 'ctrl+opt+down' });
  assert.deepEqual(panState(row, p), { kind: 'arrows', mods: ['ctrl', 'opt'] });
  assert.equal(panState(row, panPatch(row, [])).kind, 'none');
  assert.equal(panState(row, { ...p, panUp: 'f13' }).kind, 'custom');
});

test('droppedBinds finds the binds Wind ignores', () => {
  const d = droppedBinds({ zoomIn: 'mouse4,opt+a', inspect: 'ctrl+opt+i', quit: 'a', wheelZoomMods: 'ctrl+a' });
  assert.deepEqual(d.map((x) => [x.key, x.patch]), [['zoomIn', { zoomIn: 'mouse4' }], ['wheelZoomMods', { wheelZoomMods: '' }]]);
});
