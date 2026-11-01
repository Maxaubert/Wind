// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import { allRows, rowKeys, schemaDefaults } from '../src/settings-schema.js';
import { fill } from '../src/session.js';
import { themes } from '../src/design/themes.js';

// Plain node:test, no browser: run with `npm run test:rules`.
// The settings schema copies Wind for Mac's Config defaults and clamp ranges. Tests/fixtures/
// config_defaults.json is generated from Config() and clamp() by `swift run wind-checks
// --write-defaults` (and wind-checks fails when it goes stale), so a default or range changed on
// either side fails one of the two.

// Config keys with no row, on purpose.
const NO_ROW = {
  lockTracking: 'hidden until games are tested on the Mac (Config.swift)',
  panMaxLagPt: 'nothing in Sources reads it',
  onboarded: 'written by the first-run setup, not a setting',
};

function fixture() {
  const here = dirname(fileURLToPath(import.meta.url));
  // Tests/ (capital T); the same folder on the Mac's case-insensitive disk, and right on a case-sensitive one.
  return JSON.parse(readFileSync(join(here, '..', '..', 'Tests', 'fixtures', 'config_defaults.json'), 'utf8'));
}
const core = fixture().keys;
const configRows = allRows.filter((r) => rowKeys(r).length);
const decimals = (x) => (String(x).split('.')[1] || '').length;

test('every row writes real Config keys', () => {
  for (const r of configRows) for (const k of rowKeys(r)) assert.ok(core[k], `${r.key}: ${k} is not a Config key`);
});

test('every Config key has a row, or is listed as left out on purpose', () => {
  const shown = new Set(configRows.flatMap(rowKeys));
  for (const k of Object.keys(core)) assert.ok(shown.has(k) || NO_ROW[k], `${k} has no row and is not in NO_ROW`);
  for (const k of Object.keys(NO_ROW)) assert.ok(!shown.has(k), `${k} is in NO_ROW but has a row`);
});

test('schema defaults match Config()', () => {
  const d = schemaDefaults();
  for (const r of configRows) {
    for (const k of rowKeys(r)) {
      const want = core[k].def;
      const got = d[k];
      assert.notEqual(got, undefined, `${k}: no default in the schema`);
      if (core[k].type === 'int' || core[k].type === 'double') assert.equal(Number(got), Number(want), `${k} default`);
      else assert.equal(got, want, `${k} default`);
    }
  }
});

test('fill() gives an empty config the Config() defaults', () => {
  const v = fill({ values: {} });
  for (const k of configRows.flatMap(rowKeys)) {
    const t = core[k].type;
    if (t === 'int' || t === 'double') assert.equal(Number(v[k]), Number(core[k].def), k);
    else assert.equal(v[k], core[k].def, k);
  }
});

test('a slider never offers a value clamp() would change, and its default sits on a step', () => {
  for (const r of allRows.filter((x) => x.type === 'slider')) {
    const c = core[r.key];
    assert.ok(c.type === 'int' || c.type === 'double', `${r.key} is not a number in Config`);
    assert.ok(r.min >= c.min, `${r.key}: slider min ${r.min} is below clamp min ${c.min}`);
    assert.ok(r.max <= c.max, `${r.key}: slider max ${r.max} is above clamp max ${c.max}`);
    assert.ok(r.min < r.max, r.key);
    const def = Number(c.def);
    assert.ok(def >= r.min && def <= r.max, `${r.key}: default ${def} is outside ${r.min}..${r.max}`);
    const steps = (def - r.min) / r.step;
    assert.ok(Math.abs(steps - Math.round(steps)) < 1e-9, `${r.key}: default ${def} is not on a step of ${r.step} from ${r.min}`);
    if (c.type === 'int') assert.ok(Number.isInteger(r.step) && Number.isInteger(r.min), `${r.key}: an Int key needs whole steps`);
    assert.ok(decimals(r.step) <= 2, r.key);
  }
});

test('select options fit the Config type, and include the default and the clamp fallback', () => {
  for (const r of allRows.filter((x) => x.type === 'select')) {
    const c = core[r.key];
    assert.ok(r.options.includes(c.def), `${r.key}: default ${c.def} is not an option`);
    if (c.type === 'bool') assert.deepEqual([...r.options].sort(), ['0', '1'], r.key);
    else if (c.type === 'int') for (const o of r.options) {
      assert.ok(Number.isInteger(Number(o)) && Number(o) >= c.min, `${r.key}: option ${o}`);
      if (c.max > 0) assert.ok(Number(o) <= c.max, `${r.key}: option ${o} above ${c.max}`);
    }
    else if (c.type === 'string') assert.ok(r.options.includes(c.fallback), `${r.key}: clamp turns unknown values into ${c.fallback}, which is not an option`);
    else assert.fail(`${r.key}: a select on a ${c.type} key`);
    for (const o of r.options) assert.ok(r.optionLabels && r.optionLabels[o], `${r.key}: option ${o} has no label`);
  }
});

test('toggles are bool keys; keybind rows are bind lists, the wheel row a string', () => {
  for (const r of allRows.filter((x) => x.type === 'toggle')) assert.equal(core[r.key].type, 'bool', r.key);
  for (const r of allRows.filter((x) => x.type === 'keybind')) {
    for (const k of rowKeys(r)) assert.equal(core[k].type, r.wheelMods ? 'string' : 'list', k);
  }
});

test('the theme picker offers exactly Config.palettes', () => {
  assert.deepEqual(themes.map((t) => t.id), fixture().palettes);
  assert.equal(core.uiPalette.fallback, 'grey');
});
