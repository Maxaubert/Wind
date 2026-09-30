import { test, expect } from '@playwright/test';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import { checkKeyBind, checkWheelBind, checkClickBind } from '../src/lib/keybindRules.js';

// The UI mirror must agree with the C++ rules on every shared case (issue #285).
test('UI keybind rules match the shared case list', () => {
  const here = dirname(fileURLToPath(import.meta.url));
  const lines = readFileSync(join(here, '..', '..', 'tests', 'fixtures', 'keybind_cases.txt'), 'utf8').split(/\r?\n/);
  let n = 0;
  for (const line of lines) {
    if (!line || line.startsWith('#')) continue;
    const p = line.trim().split(/\s+/);
    let got;
    if (p[0] === 'key') got = checkKeyBind(parseInt(p[1], 16), Number(p[2]));
    else if (p[0] === 'wheel') got = checkWheelBind(Number(p[1]));
    else if (p[0] === 'click') got = checkClickBind(Number(p[1]), Number(p[2]));
    const want = p[p.length - 1];
    expect(got, line).toBe(want);
    n++;
  }
  expect(n).toBeGreaterThan(90);
});
