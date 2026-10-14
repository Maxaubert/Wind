import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import { GLOBAL_KEYS } from '../src/session.js';

// Plain node:test, no browser: run with `npm run test:rules` (CI runs it too).
// GLOBAL_KEYS must be the same set as the C++ kUiOnlyKeys + "profile" (src/config/profiles.h); both sides
// are checked against this shared fixture.
test('UI global keys match the shared list', () => {
  const here = dirname(fileURLToPath(import.meta.url));
  const keys = readFileSync(join(here, '..', '..', 'tests', 'fixtures', 'global_keys.txt'), 'utf8')
    .split(/\r?\n/).map((l) => l.trim()).filter((l) => l && !l.startsWith('#'));
  assert.ok(keys.length > 10, `only ${keys.length} keys read`);
  assert.deepEqual([...GLOBAL_KEYS].sort(), [...keys].sort());
});
