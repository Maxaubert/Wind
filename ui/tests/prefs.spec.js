// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
// Preferences page on the Mac: the four built-in dark themes (uiPalette), applied at once and loaded
// at launch, and the General card. Profiles are gone on the Mac.
import { test, expect } from '@playwright/test';
import { themes } from '../src/design/themes.js';
import { mockHost, open, go, row, sent, css } from './mock.js';

const app = (page) => page.locator('.app');
const token = (page, n) => app(page).evaluate((el, name) => getComputedStyle(el).getPropertyValue('--' + name).trim(), n);
const picker = (page) => row(page, 'uiPalette').getByRole('radiogroup');

test('the theme picker is one row of the four theme cards, no scrolling', async ({ page }) => {
  await mockHost(page);
  await page.setViewportSize({ width: 820, height: 700 });
  await open(page);
  await go(page, 'prefs');
  const radios = picker(page).getByRole('radio');
  expect(themes.map((t) => t.id)).toEqual(['grey', 'ember', 'ocean', 'hicon']);
  await expect(radios).toHaveCount(4);
  for (let i = 0; i < themes.length; i++) await expect(radios.nth(i)).toHaveAccessibleName(themes[i].label);
  const tops = new Set();
  for (const r of await radios.all()) tops.add(Math.round((await r.boundingBox()).y));
  expect(tops.size).toBe(1);
  expect(await picker(page).evaluate((el) => el.scrollWidth <= el.clientWidth)).toBe(true);
  await expect(picker(page).getByRole('radio', { checked: true })).toHaveAccessibleName('Wind grey');
  const bgs = new Set();
  for (const sw of await picker(page).locator('.sw').all()) bgs.add((await css(sw, 'background-color')) + '|' + (await css(sw.locator('.ac'), 'background-color')));
  expect(bgs.size).toBe(4);
});

test('theme picker keyboard: Left/Right moves and applies, End jumps, it wraps', async ({ page }) => {
  await mockHost(page);
  await open(page);
  await go(page, 'prefs');
  await picker(page).getByRole('radio', { name: 'Wind grey' }).click();
  await page.keyboard.press('ArrowRight');
  await expect(app(page)).toHaveAttribute('data-palette', 'ember');
  await expect(picker(page).getByRole('radio', { name: 'Ember' })).toBeFocused();
  await page.keyboard.press('ArrowLeft');
  await expect(app(page)).toHaveAttribute('data-palette', 'grey');
  await page.keyboard.press('End');
  await expect(app(page)).toHaveAttribute('data-palette', 'hicon');
  await page.keyboard.press('ArrowRight');
  await expect(app(page)).toHaveAttribute('data-palette', 'grey');
});

test('picking a theme restyles the window and writes uiPalette at once', async ({ page }) => {
  await mockHost(page);
  await open(page);
  await go(page, 'prefs');
  expect(await token(page, 'fill')).toBe('#2fbfa5');
  await picker(page).getByRole('radio', { name: 'Ember' }).click();
  await expect(app(page)).toHaveAttribute('data-palette', 'ember');
  expect(await token(page, 'fill')).toBe('#e48a45');
  await picker(page).getByRole('radio', { name: 'Deep ocean' }).click();
  expect(await token(page, 'fill')).toBe('#5aaaf5');
  expect((await sent(page, 'setConfig')).filter((m) => m.key === 'uiPalette').map((m) => m.value)).toEqual(['ember', 'ocean']);
  await expect(page.locator('.capsule')).toHaveCount(0);
});

test('every theme has its own block; the saved theme loads at launch', async ({ page }) => {
  await mockHost(page, { values: { uiPalette: 'hicon' } });
  await open(page);
  await expect(app(page)).toHaveAttribute('data-palette', 'hicon');
  await go(page, 'prefs');
  const seen = new Set();
  for (const t of themes) {
    await picker(page).getByRole('radio', { name: t.label }).click();
    await expect(app(page)).toHaveAttribute('data-palette', t.id);
    seen.add([await token(page, 'bg'), await token(page, 'fill'), await token(page, 'card'), await token(page, 'sel')].join('|'));
  }
  expect(seen.size).toBe(themes.length);
});

for (const id of ['nope', 'cyber', 'light', '']) {
  test(`an unknown uiPalette "${id}" falls back to Wind grey`, async ({ page }) => {
    await mockHost(page, { values: { uiPalette: id } });
    await open(page);
    await expect(app(page)).toHaveAttribute('data-palette', 'grey');
  });
}

test('the sharp radii belong to High contrast only', async ({ page }) => {
  await mockHost(page);
  await open(page);
  await go(page, 'prefs');
  for (const [id, rc] of [['hicon', '4px'], ['grey', '10px'], ['ocean', '10px'], ['ember', '10px']]) {
    await picker(page).getByRole('radio', { name: themes.find((t) => t.id === id).label }).click();
    expect(await css(page.locator('.card').first(), 'border-radius'), id).toBe(rc);
  }
});

test('General holds the theme and the advanced switch; the macOS card is last', async ({ page }) => {
  await mockHost(page);
  await open(page);
  await go(page, 'prefs');
  const keys = await page.locator('main .row').evaluateAll((rs) => rs.map((r) => r.getAttribute('data-key')));
  expect(keys).toEqual(['uiPalette', 'showAdvanced', 'warmthPct', 'dimPct', '__accessibility', '__screenRecording',
    '__zoomShortcuts', '__loginItem', 'logLevel', '__showLog', '__openIni', '__diagnostics', '__rerunSetup']);
});
