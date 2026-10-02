import { test, expect } from '@playwright/test';
import { readFileSync, readdirSync } from 'node:fs';
import { groups, allRows, groupRows, bindKeys } from '../src/settings-schema.js';

// Row keys of the pre-redesign schema (git show of settings-schema.js before #303). Every one must
// appear exactly once in the regrouped schema, except showAdvanced: that UI-only row went away
// (Advanced is always a group; the ini key stays parsed and ignored).
const OLD_KEYS = [
  '__zoomIn', '__zoomOut', '__zoomWheel', '__pan',  // was four rows (Pan left/right/up/down); their ini keys live in OLD_BIND_KEYS
  'noSwallowApps', 'maxLevel', 'zoomInSpeed', 'zoomOutSpeed', 'panSpeed', 'smoothZoomAccel',
  'smoothZoomRamp', 'zoomEaseOutMs', 'txSamplingMode', '__hideCursor', '__cursorLock', 'lockApps',
  'cursorSensitivity', 'cursorSmoothing', 'trackCaret', 'trackFocus', 'trackAlign', 'mouseAlign',
  'mouseMarginPct', 'colorWarmPct', 'colorDimPct', 'model', 'engineGame', 'engineAcrylic',
  'engineDesktop', 'engineOther', 'renderExclude', 'diagnostics', '__about',
];
// Ini keys the keybind rows own (their real state), unchanged by the regroup.
const OLD_BIND_KEYS = [
  'zoomInButton', 'zoomInVk', 'zoomInMods', 'zoomInButtonMods', 'zoomInButton2', 'zoomInVk2', 'zoomInMods2', 'zoomInButton2Mods',
  'zoomOutButton', 'zoomOutVk', 'zoomOutMods', 'zoomOutButtonMods', 'zoomOutButton2', 'zoomOutVk2', 'zoomOutMods2', 'zoomOutButton2Mods',
  'zoomWheelMods', 'panLeftVk', 'panLeftMods', 'panRightVk', 'panRightMods', 'panUpVk', 'panUpMods', 'panDownVk', 'panDownMods',
  'hideCursorVk', 'hideCursorMods', 'cursorLockVk', 'cursorLockMods',
];
const NEW_KEYS = ['__theme', '__profiles', '__diagnostics', '__openIni'];

test('group ids, order and shape follow the spec', () => {
  expect(groups.map((g) => g.id)).toEqual(['zoom', 'move', 'cursor', 'typing', 'colour', 'general', 'advanced', 'about']);
  for (const g of groups) {
    expect(typeof g.label).toBe('string');
    expect(typeof g.icon).toBe('string');
    expect(typeof g.desc).toBe('string');
    expect(g.cards.length).toBeGreaterThan(0);
    for (const c of g.cards) { expect(typeof c.caption).toBe('string'); expect(c.rows.length).toBeGreaterThan(0); }
  }
  const cap = (id) => groups.find((g) => g.id === id).cards.map((c) => c.caption);
  expect(cap('zoom')).toEqual(['Keys', 'Levels', 'Speed']);
  expect(cap('move')).toEqual(['Keys', 'Panning']);
  expect(cap('cursor')).toEqual(['Look', 'Keys']);
  expect(cap('general')).toEqual(['Appearance', 'Profiles', 'Files']);
  expect(cap('advanced')).toEqual(['Engine', 'Apps', 'Fine tuning']);
});

test('every row of the old schema appears exactly once, nothing else but the new rows', () => {
  const keys = allRows.map((r) => r.key);
  expect(new Set(keys).size).toBe(keys.length);
  for (const k of OLD_KEYS) expect(keys.filter((x) => x === k), k).toHaveLength(1);
  expect(keys).not.toContain('showAdvanced');
  expect(keys.filter((k) => !OLD_KEYS.includes(k)).sort()).toEqual([...NEW_KEYS].sort());
});

test('keybind rows keep every ini key they owned', () => {
  const owned = allRows.flatMap(bindKeys);
  expect(new Set(owned).size).toBe(owned.length);
  expect([...owned].sort()).toEqual([...OLD_BIND_KEYS].sort());
});

test('rows land in the groups the spec names', () => {
  const where = (key) => groups.find((g) => groupRows(g).some((r) => r.key === key)).id;
  expect(where('maxLevel')).toBe('zoom');
  expect(where('__zoomWheel')).toBe('zoom');
  expect(where('__pan')).toBe('move');
  expect(where('mouseAlign')).toBe('move');
  expect(where('txSamplingMode')).toBe('cursor');
  expect(where('__cursorLock')).toBe('cursor');
  expect(where('trackAlign')).toBe('typing');
  expect(where('colorDimPct')).toBe('colour');
  expect(where('__profiles')).toBe('general');
  expect(where('model')).toBe('advanced');
  expect(where('lockApps')).toBe('advanced');
  expect(where('zoomEaseOutMs')).toBe('advanced');
  expect(where('__about')).toBe('about');
});

test('per-window engines show only when the engine is Auto', () => {
  for (const k of ['engineGame', 'engineAcrylic', 'engineDesktop', 'engineOther', 'renderExclude'])
    expect(allRows.find((r) => r.key === k).showIf).toEqual({ key: 'model', eq: 'hybrid' });
  expect(allRows.find((r) => r.key === 'model').type).toBe('engine');
});

test('schema and controls contain no em-dash', () => {
  const dir = new URL('../src/controls/', import.meta.url);
  const files = readdirSync(dir).map((f) => new URL(f, dir));
  files.push(new URL('../src/settings-schema.js', import.meta.url));
  for (const f of files) expect(readFileSync(f, 'utf8'), f.pathname).not.toContain('\u2014');
});

const css = (loc, prop) => loc.evaluate((el, p) => getComputedStyle(el).getPropertyValue(p), prop);
const ctl = (page, key) => page.locator(`[data-key="${key}"]`);

test('zoom page: keycaps, slider and the old keybind safety still work', async ({ page }) => {
  await page.goto('/controls.html?group=zoom');
  await expect(page.locator('h1')).toHaveText('Zoom');
  const zin = ctl(page, '__zoomIn');
  await expect(zin.locator('.keycap').first()).toHaveText('Mouse 5+PageUp');
  await expect(zin.locator('.keycap.unbound')).toHaveText('Add key');
  await expect(zin.locator('.sep')).toHaveText('or');
  expect(await css(zin.locator('.keycap').first(), 'font-family')).toContain('Cascadia Mono');
  expect(await css(zin.locator('.keycap.unbound'), 'background-color')).toBe('rgb(14, 14, 14)');
  // capture a key on the empty slot: a live patch reaches the page
  await zin.locator('.keycap.unbound').click();
  await page.keyboard.press('F8');
  expect((await page.evaluate(() => window.__calls)).some((c) => c[0] === 'zoomInVk2' && c[1] === '119')).toBe(true);
  // slider: end-cap thumb and value readout
  const sl = ctl(page, 'maxLevel').locator('input[type=range]');
  await expect(sl).toHaveAttribute('aria-valuetext', '12 times');
  expect(parseFloat(await css(sl, '--pct'))).toBeCloseTo(20.83, 1);
  await sl.fill('30');
  await expect(ctl(page, 'maxLevel').locator('.val')).toHaveText('30x');
});

test('toggle and select drive their row value', async ({ page }) => {
  await page.goto('/controls.html?group=typing');
  const t = ctl(page, 'trackFocus').getByRole('switch');
  await expect(t).not.toBeChecked();
  await t.check({ force: true });
  await ctl(page, 'trackAlign').locator('select').selectOption('1');
  expect(await page.evaluate(() => window.__calls)).toEqual([['trackFocus', 1], ['trackAlign', '1']]);
  await expect(ctl(page, 'trackAlign').locator('select')).toHaveValue('1');
});

test('advanced: engine row offers Restart Wind, per-window rows follow Auto, app list manages apps', async ({ page }) => {
  await page.goto('/controls.html?group=advanced');
  await expect(ctl(page, 'engineGame')).toBeVisible();
  await expect(page.getByRole('button', { name: 'Restart Wind' })).toHaveCount(0);
  await ctl(page, 'model').locator('select').selectOption('render');
  await expect(page.getByRole('button', { name: 'Restart Wind' })).toBeVisible();
  await expect(ctl(page, 'engineGame')).toHaveCount(0);
  await expect(ctl(page, 'renderExclude')).toHaveCount(0);
  await page.getByRole('button', { name: 'Restart Wind' }).click();
  await ctl(page, 'model').locator('select').selectOption('hybrid');
  await expect(page.getByRole('button', { name: 'Restart Wind' })).toHaveCount(0);
  const apps = ctl(page, 'renderExclude');
  await expect(apps.locator('.sum')).toHaveText('netflix.exe');
  await apps.getByRole('button', { name: /Manage/ }).click();
  await page.getByRole('button', { name: 'Add program...' }).click();
  await expect(apps.locator('.sum')).toHaveText('2 apps');
  await page.getByRole('button', { name: 'Remove netflix.exe' }).click();
  await expect(apps.locator('.sum')).toHaveText('RDR2.exe');
  await page.keyboard.press('Escape');
  await expect(page.getByRole('dialog')).toHaveCount(0);
  const calls = await page.evaluate(() => window.__calls);
  expect(calls).toContainEqual(['renderExclude', 'netflix.exe,RDR2.exe']);
  expect(calls).toContain('restart');
});

test('cursor page: high resolution toggle and inspect keycap', async ({ page }) => {
  await page.goto('/controls.html?group=cursor');
  await ctl(page, 'txSamplingMode').getByRole('switch').check({ force: true });
  expect(await page.evaluate(() => window.__calls)).toEqual([['txSamplingMode', 1]]);
  await expect(ctl(page, '__cursorLock').locator('.keycap')).toHaveText('Add key');
});

test('general page: theme, profiles and file buttons', async ({ page }) => {
  await page.goto('/controls.html?group=general');
  await ctl(page, '__theme').getByRole('radio', { name: 'Light' }).click();
  await ctl(page, '__diagnostics').getByRole('button', { name: 'Export' }).click();
  await ctl(page, '__openIni').getByRole('button', { name: 'Open' }).click();
  const prof = ctl(page, '__profiles');
  await prof.locator('select').selectOption('Default');
  await prof.getByRole('button', { name: 'New' }).click();
  await prof.getByLabel('Profile name').fill('Gaming');
  await prof.getByRole('button', { name: 'OK' }).click();
  await expect(prof.getByRole('alert')).toHaveText('A profile with that name already exists');
  await prof.getByLabel('Profile name').fill('Work');
  await prof.getByRole('button', { name: 'OK' }).click();
  expect(await page.evaluate(() => window.__calls)).toEqual([
    ['theme', 'light'], ['action', 'exportDiagnostics'], ['action', 'openIni'], ['switch', 'Default'], ['create', 'Work'],
  ]);
});

test('about page shows the logo and link; light theme controls read on white', async ({ page }) => {
  await page.goto('/controls.html?group=about&theme=light');
  await expect(page.getByText('Barely there. Everywhere.')).toBeVisible();
  await page.getByRole('button', { name: 'Star on GitHub' }).click();
  expect(await page.evaluate(() => window.__calls)).toEqual(['repo']);
  await page.goto('/controls.html?group=colour&theme=light');
  const sl = ctl(page, 'colorWarmPct').locator('input');
  expect(await css(sl, 'accent-color')).not.toBe('');
  expect(await css(ctl(page, 'colorWarmPct').locator('.val'), 'color')).toBe('rgb(10, 10, 10)');
});
