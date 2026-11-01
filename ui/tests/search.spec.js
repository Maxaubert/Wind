// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
// Settings search on the Mac schema: the pure ranker and fuzzy matcher, then the results page against
// the mock host (ranked list, the real controls edited in place, advanced and gated rows, Cmd+F).
import { test, expect } from '@playwright/test';
import { groups, allRows } from '../src/settings-schema.js';
import { search, editDistance, words } from '../src/search/search.js';
import { mockHost, open, go, row, live, CMD } from './mock.js';

test.beforeEach(async ({ page }) => { await mockHost(page, { values: { maxLevel: '12' } }); });

const input = (page) => page.getByLabel('Search settings');
const keys = (q) => search(groups, q).map((h) => h.key);
const rank = (q, key) => { const i = keys(q).indexOf(key); return i < 0 ? 999 : i; };

// ---- the pure matcher -------------------------------------------------------------------------

test('words() ignores case, accents and punctuation; editDistance counts swaps as one', () => {
  expect(words('Zoom-in  SPEED!')).toEqual(['zoom', 'in', 'speed']);
  expect(words('Café')).toEqual(['cafe']);
  expect(editDistance('centre', 'center')).toBe(1);
  expect(editDistance('warmth', 'brightness', 2)).toBeGreaterThan(2);
});

test('empty and unmatched queries return nothing; every word must match', () => {
  expect(search(groups, '')).toEqual([]);
  expect(search(groups, '  ,, ')).toEqual([]);
  expect(keys('xyzzy')).toEqual([]);
  expect(keys('zoom xyzzy')).toEqual([]);
});

test('results are one flat list ranked best first, with the page name on each hit', () => {
  const hits = search(groups, 'zoom');
  expect(hits.length).toBeGreaterThan(5);
  for (let i = 1; i < hits.length; i++) expect(hits[i - 1].score).toBeGreaterThanOrEqual(hits[i].score);
  expect(hits[0].groupLabel).toBeTruthy();
  expect(hits[0].row.key).toBe(hits[0].key);
});

test('ranking: exact label first, label beats keyword', () => {
  expect(keys('zoom in')[0]).toBe('zoomIn');
  expect(keys('engine')[0]).toBe('engine');
  expect(keys('warmth')[0]).toBe('warmthPct');
  expect(keys('theme')).toEqual(['uiPalette']);
  expect(keys('brigh')[0]).toBe('dimPct');
  expect(keys('max zoom')[0]).toBe('maxLevel');
  expect(keys('mouse speed')[0]).toBe('cursorSensitivity');
  expect(keys('soft start length')[0]).toBe('smoothZoomRamp');
  expect(rank('speed', 'zoomInSpeed')).toBeLessThan(rank('speed', 'zoomEaseOutMs'));
  expect(keys('preferences')).toContain('uiPalette');   // the page name still finds rows
});

test('typo tolerance: transpositions, dropped letters, phonetic spellings', () => {
  expect(keys('warmht')).toContain('warmthPct');
  expect(keys('brigthness')[0]).toBe('dimPct');
  expect(keys('brihtness')[0]).toBe('dimPct');
  expect(keys('inertai')).toContain('zoomEaseOutMs');
  expect(keys('sensitivty')).toContain('cursorSensitivity');
  expect(keys('nite light')[0]).toBe('warmthPct');
  expect(keys('mouze')).toContain('cursorSensitivity');
  expect(keys('MAX ZO')).toContain('maxLevel');
  expect(keys('zoomin')).toContain('zoomIn');
  expect(keys('xom')).toEqual([]);   // 3 letters: no typo budget
});

test('keywords reach rows without their label words', () => {
  expect(keys('center')).toEqual(expect.arrayContaining(['mouseAlign', 'trackAlign']));
  expect(keys('mouse position')[0]).toBe('mouseAlign');
  expect(keys('magnify')).toContain('zoomIn');
  expect(keys('hotkey')).toEqual(expect.arrayContaining(['zoomIn', 'zoomOut', 'quickZoom', '__pan']));
  expect(keys('night light')[0]).toBe('warmthPct');
  expect(keys('dim')).toContain('dimPct');
  expect(keys('renderer')).toContain('engine');
  expect(keys('freeze')).toContain('inspect');
  expect(keys('crosshair')).toContain('inspect');
  expect(keys('arrows')).toContain('__pan');
  expect(keys('momentum')).toContain('zoomEaseOutMs');
});

test('Mac words find the macOS card', () => {
  expect(keys('permission')).toEqual(expect.arrayContaining(['__accessibility', '__screenRecording']));
  expect(keys('login')[0]).toBe('__loginItem');
  expect(keys('startup')).toContain('__loginItem');
  expect(keys('screen recording')[0]).toBe('__screenRecording');
  expect(keys('black screen')).toContain('__screenRecording');
  expect(keys('accessibility')[0]).toBe('__accessibility');
  expect(keys('log')).toEqual(expect.arrayContaining(['logLevel', '__showLog']));
  expect(keys('macos')).toContain('__zoomShortcuts');
  expect(keys('onboarding')).toContain('__rerunSetup');
});

test('every labelled row has keywords, none empty', () => {
  const rows = allRows.filter((r) => r.label);
  expect(rows.length).toBeGreaterThan(30);
  for (const r of rows) {
    expect(Array.isArray(r.keywords) && r.keywords.length >= 3, r.key + ' needs keywords').toBe(true);
    for (const k of r.keywords) expect(words(k).length, r.key + ' keyword ' + k).toBeGreaterThan(0);
  }
});

test('advanced rows are searchable; the about row is not indexed', () => {
  expect(keys('advanced')).toEqual(expect.arrayContaining(['showAdvanced', 'smoothZoomAccel']));
  expect(keys('logo')).not.toContain('__about');
  expect(keys('github')).toEqual([]);
  expect(keys('profile')).toEqual([]);
});

// ---- the results page --------------------------------------------------------------------------

test('typing shows one ranked list of real rows with a quiet page name each', async ({ page }) => {
  await open(page);
  await input(page).fill('zoom');
  await expect(page.locator('.res')).toBeVisible();
  const shown = await page.locator('.res .hit').evaluateAll((els) => els.map((e) => e.getAttribute('data-hit')));
  expect(shown).toEqual(keys('zoom'));
  await expect(page.locator('.res .hit[data-hit="maxLevel"] .tab')).toHaveText('Zoom');
  await expect(page.locator('.res .hit[data-hit="zoomIn"] .tab')).toHaveText('Hotkeys');
  await expect(page.locator('.res .hit[data-hit="maxLevel"] input[type=range]')).toBeVisible();
  await expect(page.locator('.res .hit[data-hit="zoomIn"] .bx')).toBeVisible();
  await expect(page.locator('main')).toHaveAttribute('aria-label', 'Search results');
  await expect(page.locator('.side .it.sel')).toHaveCount(0);
});

test('a result control changes the setting in place and the page shows it after', async ({ page }) => {
  await open(page);
  await input(page).fill('keyboard focus');
  const sw = page.locator('.res .hit[data-hit="trackFocus"]').getByRole('switch');
  await expect(sw).not.toBeChecked();
  await sw.click({ force: true });
  await expect(sw).toBeChecked();
  await expect.poll(() => live(page, 'trackFocus')).toBe('1');
  await expect(page.locator('.res')).toBeVisible();
  await input(page).press('Escape');
  await go(page, 'view');
  await expect(row(page, 'trackFocus').getByRole('switch')).toBeChecked();
});

test('a select and a slider edit in place too', async ({ page }) => {
  await open(page);
  await input(page).fill('mouse position');
  await page.locator('.res .hit[data-hit="mouseAlign"] select').selectOption('1');
  await expect.poll(() => live(page, 'mouseAlign')).toBe('1');
  await input(page).fill('max zoom');
  const sl = page.locator('.res .hit[data-hit="maxLevel"] input[type=range]');
  await sl.focus();
  await page.keyboard.press('ArrowRight');
  await expect.poll(() => live(page, 'maxLevel')).toBe('13');
});

test('advanced rows are editable from search while Show advanced settings is off', async ({ page }) => {
  await open(page);
  await input(page).fill('soft start');
  const accel = page.locator('.res .hit[data-hit="smoothZoomAccel"]');
  await expect(accel).toBeVisible();
  await accel.locator('input[type=range]').focus();
  await page.keyboard.press('ArrowRight');
  await expect.poll(() => live(page, 'smoothZoomAccel')).toBe('3.5');
  await input(page).fill('engine');
  await expect(page.locator('.res .hit[data-hit="engine"] select')).toBeVisible();
});

test('a row gated by another setting shows dimmed, not hidden', async ({ page }) => {
  await open(page);
  await input(page).fill('edge margin');
  await expect(page.locator('.res .hit[data-hit="mouseMarginPct"] .row')).toHaveClass(/disabled/);
  // The macOS zoom rows are dimmed on the render engine.
  await input(page).fill('keep pointer speed');
  await expect(page.locator('.res .hit[data-hit="keepPointerSpeed"] .row')).toHaveClass(/disabled/);
});

test('Esc clears, Cmd+F focuses, Enter moves into the first result, Tab walks on', async ({ page, browserName }) => {
  await open(page);
  await page.locator('main').click({ position: { x: 5, y: 5 } });
  await page.keyboard.press(CMD + '+f');
  await expect(input(page)).toBeFocused();
  await input(page).fill('speed');
  await input(page).press('Enter');
  await expect(page.locator('.res .hit[data-hit="' + keys('speed')[0] + '"] input').first()).toBeFocused();
  // Safari and WKWebView: Tab reaches only text fields unless the Mac's keyboard navigation is on; Option+Tab reaches everything.
  await page.keyboard.press(browserName === 'webkit' ? 'Alt+Tab' : 'Tab');
  await expect(page.locator('.res .hit[data-hit="' + keys('speed')[1] + '"] input').first()).toBeFocused();
  await page.keyboard.press('Escape');
  await expect(page.locator('.res')).toHaveCount(0);
  await expect(input(page)).toHaveValue('');
  await expect(page.locator('h1')).toHaveText('Hotkeys');
});

test('no matches says so', async ({ page }) => {
  await open(page);
  await input(page).fill('qqqq');
  await expect(page.locator('.res .sum')).toContainText('No settings match');
  await expect(page.locator('.res .hit')).toHaveCount(0);
});
