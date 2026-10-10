import { test, expect } from '@playwright/test';
import { readFileSync, readdirSync } from 'node:fs';
import { groups, allRows, groupRows, bindKeys } from '../src/settings-schema.js';

// Row keys of the #303 schema. Every one must still appear exactly once after #318, except the two
// REMOVED rows: the scroll-wheel row (the wheel is a Zoom in / Zoom out binding now) and "Never use
// Render for" (renderExclude stays in the ini and the core, not in Settings).
const REMOVED = ['__zoomWheel', 'renderExclude'];
const OLD_KEYS = [
  '__zoomIn', '__zoomOut', '__zoomWheel', '__pan',  // was four rows (Pan left/right/up/down); their ini keys live in OLD_BIND_KEYS
  'noSwallowApps', 'maxLevel', 'zoomInSpeed', 'zoomOutSpeed', 'panSpeed', 'smoothZoomAccel',
  'smoothZoomRamp', 'zoomEaseOutMs', 'txSamplingMode', '__hideCursor', '__cursorLock', 'lockApps',
  'cursorSensitivity', 'panGlideMaxPx', 'trackCaret', 'trackFocus', 'trackAlign', 'mouseAlign',
  'mouseMarginPct', 'colorWarmPct', 'colorDimPct', 'model', 'engineGame', 'engineAcrylic',
  'engineDesktop', 'engineOther', 'renderExclude', 'diagnostics', '__about',
];
// Ini keys the keybind rows own (their real state), unchanged by the regroup.
const OLD_BIND_KEYS = [
  'zoomInButton', 'zoomInVk', 'zoomInMods', 'zoomInButtonMods', 'zoomInButton2', 'zoomInVk2', 'zoomInMods2', 'zoomInButton2Mods',
  'zoomOutButton', 'zoomOutVk', 'zoomOutMods', 'zoomOutButtonMods', 'zoomOutButton2', 'zoomOutVk2', 'zoomOutMods2', 'zoomOutButton2Mods',
  'panLeftVk', 'panLeftMods', 'panRightVk', 'panRightMods', 'panUpVk', 'panUpMods', 'panDownVk', 'panDownMods',
  'hideCursorVk', 'hideCursorMods', 'cursorLockVk', 'cursorLockMods',
];
const NEW_KEYS = ['uiPalette', '__profiles', '__diagnostics', '__openIni', 'trayPerf', 'trayPinned', 'showAdvanced'];

test('group ids, order and shape follow the spec', () => {
  expect(groups.map((g) => g.id)).toEqual(['hotkeys', 'zoom', 'view', 'prefs', 'tray', 'about']);
  expect(groups.filter((g) => g.bottom).map((g) => g.id)).toEqual(['prefs', 'tray', 'about']);   // below the divider
  for (const g of groups) {
    expect(typeof g.label).toBe('string');
    expect(typeof g.icon).toBe('string');
    expect(typeof g.desc).toBe('string');
    expect(g.cards.length).toBeGreaterThan(0);
    for (const c of g.cards) { expect(typeof c.caption).toBe('string'); expect(c.rows.length).toBeGreaterThan(0); }
  }
  const cap = (id) => groups.find((g) => g.id === id).cards.map((c) => c.caption);
  expect(cap('hotkeys')).toEqual(['Zoom', 'Extra keys']);
  expect(cap('zoom')).toEqual(['Level and speed', 'Easing', 'Engine']);
  expect(cap('view')).toEqual(['Speed', 'Pointer', 'Typing and focus']);
  expect(cap('prefs')).toEqual(['General', 'Screen light', 'Troubleshooting']);
  expect(cap('tray')).toEqual(['']);
});

test('every row of the old schema appears exactly once, nothing else but the new rows', () => {
  const keys = allRows.map((r) => r.key);
  expect(new Set(keys).size).toBe(keys.length);
  for (const k of OLD_KEYS.filter((x) => !REMOVED.includes(x))) expect(keys.filter((x) => x === k), k).toHaveLength(1);
  for (const k of REMOVED) expect(keys, k).not.toContain(k);
  expect(keys.filter((k) => !OLD_KEYS.includes(k)).sort()).toEqual([...NEW_KEYS].sort());
});

test('a captioned section has two or more rows, and the advanced rows are the ones the spec names', () => {
  for (const g of groups) for (const c of g.cards) if (c.caption) expect(c.rows.length, g.id + ' / ' + c.caption).toBeGreaterThanOrEqual(2);
  // With the switch off every captioned section still keeps two or more rows (an adv row never carries a section alone).
  for (const g of groups) for (const c of g.cards) if (c.caption) {
    const base = c.rows.filter((r) => !r.adv && (!r.showIf || r.showIf.key !== 'mouseAlign'));
    if (base.length) expect(base.length, g.id + ' / ' + c.caption + ' (advanced off)').toBeGreaterThanOrEqual(2);
  }
  expect(allRows.filter((r) => r.adv).map((r) => r.key).sort()).toEqual(
    ['engineAcrylic', 'engineDesktop', 'engineGame', 'engineOther', 'lockApps', 'model', 'noSwallowApps', 'smoothZoomAccel', 'smoothZoomRamp'].sort());
  // Troubleshooting is never advanced, and the extra keys carry their switch key.
  for (const k of ['diagnostics', '__diagnostics', '__openIni']) expect(allRows.find((r) => r.key === k).adv).toBeUndefined();
  expect(Object.fromEntries(allRows.filter((r) => r.onKey).map((r) => [r.key, r.onKey]))).toEqual(
    { __pan: 'panKeysOn', __hideCursor: 'hideCursorOn', __cursorLock: 'cursorLockOn' });
  expect(allRows.find((r) => r.key === '__zoomIn').max).toBe(2);
  expect(allRows.find((r) => r.key === '__zoomOut').max).toBe(2);
});

test('copy follows the ia07 table, with no em-dash and no old names', () => {
  const label = (k) => allRows.find((r) => r.key === k).label;
  expect(label('__hideCursor')).toBe('Hide pointer');
  expect(label('panSpeed')).toBe('Arrow key speed');
  expect(label('cursorSensitivity')).toBe('Mouse speed');
  expect(label('mouseAlign')).toBe('Pointer position');
  expect(label('trackAlign')).toBe('Text cursor position');
  expect(label('smoothZoomAccel')).toBe('Soft start');
  expect(label('smoothZoomRamp')).toBe('Soft start length');
  expect(label('model')).toBe('Engine');
  expect(label('noSwallowApps')).toBe('Share zoom keys with these apps');
  expect(label('diagnostics')).toBe('Frame time logging');
  expect(label('showAdvanced')).toBe('Show advanced settings');
  for (const r of allRows) expect(JSON.stringify([r.label, r.desc]), r.key).not.toMatch(/flyout|\u2014/);
});

test('keybind rows keep every ini key they owned', () => {
  const owned = allRows.flatMap(bindKeys);
  expect(new Set(owned).size).toBe(owned.length);
  expect([...owned].sort()).toEqual([...OLD_BIND_KEYS].sort());
});

test('rows land in the groups the spec names', () => {
  const where = (key) => groups.find((g) => groupRows(g).some((r) => r.key === key)).id;
  expect(where('maxLevel')).toBe('zoom');
  expect(where('zoomEaseOutMs')).toBe('zoom');
  expect(where('model')).toBe('zoom');
  expect(where('__zoomIn')).toBe('hotkeys');
  expect(where('noSwallowApps')).toBe('hotkeys');
  expect(where('__pan')).toBe('hotkeys');
  expect(where('__hideCursor')).toBe('hotkeys');
  expect(where('__cursorLock')).toBe('hotkeys');
  expect(where('panSpeed')).toBe('view');
  expect(where('mouseAlign')).toBe('view');
  expect(where('txSamplingMode')).toBe('view');
  expect(where('lockApps')).toBe('view');
  expect(where('trackAlign')).toBe('view');
  expect(where('colorDimPct')).toBe('prefs');
  expect(where('__profiles')).toBe('prefs');
  expect(where('showAdvanced')).toBe('prefs');
  expect(where('diagnostics')).toBe('prefs');
  expect(where('__about')).toBe('about');
});

test('per-window engines show only when the engine is Auto', () => {
  for (const k of ['engineGame', 'engineAcrylic', 'engineDesktop', 'engineOther'])
    expect(allRows.find((r) => r.key === k).showIf).toEqual({ key: 'model', eq: 'hybrid' });
  expect(allRows.find((r) => r.key === 'model').type).toBe('engine');
  expect(allRows.find((r) => r.key === 'mouseMarginPct').showIf).toEqual({ key: 'mouseAlign', eq: '1' });   // the margin only matters when the pointer is kept within the edges
});

test('schema and controls contain no em-dash', () => {
  const dir = new URL('../src/controls/', import.meta.url);
  const files = readdirSync(dir).map((f) => new URL(f, dir));
  files.push(new URL('../src/settings-schema.js', import.meta.url));
  for (const f of files) expect(readFileSync(f, 'utf8'), f.pathname).not.toContain('\u2014');
});

const css = (loc, prop) => loc.evaluate((el, p) => getComputedStyle(el).getPropertyValue(p), prop);
const ctl = (page, key) => page.locator(`[data-key="${key}"]`);

test('hotkeys page: one box per binding, "or" between them, and the old keybind safety still works', async ({ page }) => {
  await page.goto('/controls.html?group=hotkeys');
  await expect(page.locator('h1')).toHaveText('Hotkeys');
  const zin = ctl(page, '__zoomIn');
  // The harness holds a button AND a key in one slot (an older ini): two boxes, none left to add.
  await expect(zin.locator('.kb .kc')).toHaveText(['Mouse 5', 'PageUp']);
  await expect(zin.locator('.pl')).toHaveText('or');
  await expect(zin.locator('.kc.ghost')).toHaveCount(0);
  expect(await css(zin.locator('.kb .kc').first(), 'font-family')).toContain('Cascadia Mono');
  // Zoom out holds nothing: one ghost, and recording a key on it reaches the page as a live patch.
  const zout = ctl(page, '__zoomOut');
  await expect(zout.locator('.kc.ghost')).toHaveText('Set key');
  expect(await css(zout.locator('.kc.ghost'), 'background-color')).toBe('rgb(14, 14, 14)');
  await zout.locator('.kc.ghost').click();
  await page.keyboard.press('F8');
  expect((await page.evaluate(() => window.__calls)).some((c) => c[0] === 'zoomOutVk' && c[1] === '119')).toBe(true);
  await expect(zout.locator('.kb .kc')).toHaveText(['F8']);
  await expect(zout.locator('.kc.ghost')).toHaveText('Add key');   // zoom rows take a second binding
  // slider: end-cap thumb and value readout
  await page.goto('/controls.html?group=zoom');
  const sl = ctl(page, 'maxLevel').locator('input[type=range]');
  await expect(sl).toHaveAttribute('aria-valuetext', '12 times');
  expect(parseFloat(await css(sl, '--pct'))).toBeCloseTo(20.83, 1);
  await sl.fill('30');
  await expect(ctl(page, 'maxLevel').locator('.val')).toHaveText('30x');
});

test('toggle and select drive their row value', async ({ page }) => {
  await page.goto('/controls.html?group=view');
  const t = ctl(page, 'trackFocus').getByRole('switch');
  await expect(t).not.toBeChecked();
  await t.check({ force: true });
  await ctl(page, 'trackAlign').locator('select').selectOption('1');
  expect(await page.evaluate(() => window.__calls)).toEqual([['trackFocus', 1], ['trackAlign', '1']]);
  await expect(ctl(page, 'trackAlign').locator('select')).toHaveValue('1');
});

test('engine row offers Restart Wind and per-window rows follow Auto; the share-keys app list manages apps', async ({ page }) => {
  await page.goto('/controls.html?group=zoom');
  await expect(ctl(page, 'engineGame')).toBeVisible();
  await expect(page.getByRole('button', { name: 'Restart Wind' })).toHaveCount(0);
  await ctl(page, 'model').locator('select').selectOption('render');
  await expect(page.getByRole('button', { name: 'Restart Wind' })).toBeVisible();
  await expect(ctl(page, 'engineGame')).toHaveCount(0);
  await page.getByRole('button', { name: 'Restart Wind' }).click();
  await ctl(page, 'model').locator('select').selectOption('hybrid');
  await expect(page.getByRole('button', { name: 'Restart Wind' })).toHaveCount(0);
  await page.goto('/controls.html?group=hotkeys');
  const apps = ctl(page, 'noSwallowApps');
  await expect(apps.locator('.sum')).toHaveText('netflix.exe');
  await apps.getByRole('button', { name: /Manage/ }).click();
  await page.getByRole('button', { name: 'Add program...' }).click();
  await expect(apps.locator('.sum')).toHaveText('2 apps');
  await page.getByRole('button', { name: 'Remove netflix.exe' }).click();
  await expect(apps.locator('.sum')).toHaveText('RDR2.exe');
  await page.keyboard.press('Escape');
  await expect(page.getByRole('dialog')).toHaveCount(0);
  const calls = await page.evaluate(() => window.__calls);
  expect(calls).toContainEqual(['noSwallowApps', 'netflix.exe,RDR2.exe']);
});

test('view page: high resolution toggle; hotkeys page: inspect box and its switch', async ({ page }) => {
  await page.goto('/controls.html?group=view');
  await ctl(page, 'txSamplingMode').getByRole('switch').check({ force: true });
  expect(await page.evaluate(() => window.__calls)).toEqual([['txSamplingMode', 1]]);
  await page.goto('/controls.html?group=hotkeys');
  const ins = ctl(page, '__cursorLock');
  await expect(ins.locator('.kc.ghost')).toHaveText('Set key');
  await expect(ins.getByRole('switch')).toBeChecked();   // extra keys ship on
  await ins.getByRole('switch').uncheck({ force: true });
  expect(await page.evaluate(() => window.__calls)).toEqual([['cursorLockOn', 0]]);
  await expect(ins.locator('.kwrap')).toHaveClass(/off/);   // the binding dims, it is not lost
});

test('preferences page: theme, profiles and file buttons', async ({ page }) => {
  await page.goto('/controls.html?group=prefs');
  await ctl(page, '__diagnostics').getByRole('button', { name: 'Export' }).click();
  await ctl(page, '__openIni').getByRole('button', { name: 'Open' }).click();
  await ctl(page, 'uiPalette').getByRole('radio', { name: 'Ember' }).click();
  const prof = ctl(page, '__profiles');
  await prof.getByRole('button', { name: /^Profile:/ }).click();
  await prof.getByRole('option', { name: 'Default' }).click();
  await prof.getByRole('button', { name: 'New' }).click();
  await prof.getByRole('button', { name: /^Profile:/ }).click();
  await prof.getByRole('button', { name: 'Delete profile Gaming' }).click();
  expect(await page.evaluate(() => window.__calls)).toEqual([
    ['action', 'exportDiagnostics'], ['action', 'openIni'], ['uiPalette', 'ember'],
    ['switch', 'Default'], 'new', ['delete', 'Gaming'],
  ]);
});

test('about page shows the logo and link; controls read on the dark surface', async ({ page }) => {
  await page.goto('/controls.html?group=about');
  await expect(page.getByText('Barely there. Everywhere.')).toBeVisible();
  await page.getByRole('button', { name: 'Star on GitHub' }).click();
  expect(await page.evaluate(() => window.__calls)).toEqual(['repo']);
  await page.goto('/controls.html?group=prefs');
  const sl = ctl(page, 'colorWarmPct').locator('input');
  expect(await css(sl, 'accent-color')).not.toBe('');
  expect(await css(ctl(page, 'colorWarmPct').locator('.val'), 'color')).toBe('rgb(242, 242, 242)');
});
