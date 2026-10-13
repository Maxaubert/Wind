import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import { allRows } from '../src/settings-schema.js';
import { fill, GLOBAL_KEYS } from '../src/session.js';

// Plain node:test, no browser: run with `npm run test:rules` (CI runs it too).
// The Settings schema copies the core's defaults and slider ranges. tests/fixtures/config_defaults.txt
// is generated from Config{} and ParseConfig's clamps by tests/test_config_defaults.cpp (which also
// fails when the fixture goes stale), so a default changed on either side fails one of the two tests.

// Keys the page owns: the core never parses them, so they have no row in the fixture.
const UI_ONLY = new Set(['uiPalette', 'showAdvanced', 'trayPerf']);

// Sliders that deliberately offer less than the core accepts from a hand-edited ini. A slider may be
// narrower than the core's clamp, never wider; list a narrower one here with the reason.
const NARROWER = {
  maxLevel: 'the slider starts at 2x; the core clamp starts at 1x, the minimum zoom level',
  panGlideMaxPx: 'the slider stops at 200 px; the core accepts up to 400 from the ini',
};

function readFixture() {
  const here = dirname(fileURLToPath(import.meta.url));
  const text = readFileSync(join(here, '..', '..', 'tests', 'fixtures', 'config_defaults.txt'), 'utf8');
  const core = new Map();
  for (const line of text.split(/\r?\n/)) {
    if (!line || line.startsWith('#')) continue;
    const [key, min, max, ...rest] = line.split('\t');
    core.set(key, { min: min === '-' ? null : Number(min), max: max === '-' ? null : Number(max), def: rest.join('\t') });
  }
  return core;
}

// Same value as the host's SameIniValue: identical text, or both plain numbers with the same value.
function same(a, b) {
  const x = String(a ?? '').trim(), y = String(b ?? '').trim();
  if (x === y) return true;
  if (x === '' || y === '') return false;
  return Number.isFinite(Number(x)) && Number.isFinite(Number(y)) && Number(x) === Number(y);
}

const core = readFixture();

test('the fixture was read', () => {
  assert.ok(core.size > 100, `only ${core.size} keys read`);
  assert.ok(core.has('maxLevel') && core.has('model'));
});

test('every default the page fills in matches the core default', () => {
  // fill() seeds every schema row's def plus the keybind defaults (KB_DEFAULTS in session.js).
  const defaults = fill({ values: {} }).values;
  let n = 0;
  for (const [key, def] of Object.entries(defaults)) {
    if (UI_ONLY.has(key)) {
      assert.ok(!core.has(key), `${key} is listed as UI-only but the core parses it`);
      assert.ok(GLOBAL_KEYS.has(key), `${key} is UI-only, so it must also be a global (non-profile) key`);
      continue;
    }
    assert.ok(core.has(key), `${key}: the schema has a default but the core does not parse this key`);
    assert.ok(same(def, core.get(key).def), `${key}: schema default ${JSON.stringify(def)}, core default ${JSON.stringify(core.get(key).def)}`);
    n++;
  }
  assert.ok(n > 50, `only ${n} defaults compared`);
});

test('slider ranges stay inside the core clamp, and only listed sliders are narrower', () => {
  let n = 0;
  for (const r of allRows) {
    if (r.type !== 'slider') continue;
    const c = core.get(r.key);
    assert.ok(c, `${r.key}: slider for a key the core does not parse`);
    assert.ok(c.min !== null && c.max !== null, `${r.key}: slider for a key the core does not clamp on both sides`);
    assert.ok(r.min >= c.min && r.max <= c.max, `${r.key}: slider ${r.min}..${r.max} is wider than the core's ${c.min}..${c.max}`);
    assert.ok(r.def >= r.min && r.def <= r.max, `${r.key}: default ${r.def} outside the slider's ${r.min}..${r.max}`);
    const narrower = r.min !== c.min || r.max !== c.max;
    if (narrower) assert.ok(r.key in NARROWER, `${r.key}: slider ${r.min}..${r.max} is narrower than the core's ${c.min}..${c.max}; add it to NARROWER with the reason`);
    else assert.ok(!(r.key in NARROWER), `${r.key}: listed in NARROWER but matches the core range now; remove it`);
    n++;
  }
  for (const key of Object.keys(NARROWER)) assert.ok(allRows.some((r) => r.key === key && r.type === 'slider'), `${key}: in NARROWER but not a slider`);
  assert.ok(n > 10, `only ${n} sliders checked`);
});
