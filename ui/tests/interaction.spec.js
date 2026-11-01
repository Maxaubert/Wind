// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
// How the window feels: no text selection outside the search field, focus rings for the keyboard
// only, short motion that turns off under reduced motion.
import { test, expect } from '@playwright/test';
import { mockHost, open, go, row, css } from './mock.js';

test.beforeEach(async ({ page }) => { await mockHost(page, { refuse: ['trackFocus'] }); });

const noRing = async (loc) => {
  const style = await css(loc, 'outline-style');
  const width = await css(loc, 'outline-width');
  expect(style === 'none' || parseFloat(width) === 0).toBe(true);
};
const tiny = (v) => v.split(',').every((d) => parseFloat(d) < 0.001);

test('text cannot be selected, except inside the search field', async ({ page }) => {
  await open(page);
  const label = page.locator('.row .label').first();
  expect(await css(label, 'user-select')).toBe('none');
  expect(await css(page.locator('.side'), 'user-select')).toBe('none');
  await label.dblclick();
  expect(await page.evaluate(() => String(getSelection()))).toBe('');
  const search = page.getByPlaceholder('Search settings');
  expect(await css(search, 'user-select')).toBe('text');
  await search.fill('zoom level');
  await search.selectText();
  expect(await page.evaluate(() => { const i = document.activeElement; return i.value.slice(i.selectionStart, i.selectionEnd); })).toBe('zoom level');
});

test('a mouse click leaves no focus ring on a control', async ({ page }) => {
  await open(page);
  const box = row(page, 'hideCursor').locator('.ghost');
  await box.click();
  await noRing(box);
  await page.mouse.click(600, 20);   // leave capture with the mouse
  await go(page, 'zoom');
  const slider = row(page, 'maxLevel').locator('input[type=range]');
  await slider.click();
  await noRing(slider);
  await noRing(page.locator('.side .it[data-g="zoom"]'));
  await go(page, 'view');
  const toggle = row(page, 'trackCaret').getByRole('switch');
  await toggle.click({ force: true });
  await noRing(toggle.locator('xpath=following-sibling::*[1]'));
});

test('reduced motion: the dialog and page fade carry no animation', async ({ page }) => {
  await page.emulateMedia({ reducedMotion: 'reduce' });
  await open(page);
  await go(page, 'view');
  expect(tiny(await css(page.locator('.page'), 'animation-duration'))).toBe(true);
  await row(page, 'trackFocus').getByRole('switch').click({ force: true });
  const box = page.locator('.mbox');
  await expect(box).toBeVisible();
  expect(tiny(await css(box, 'animation-duration'))).toBe(true);
});

test('normal motion: the dialog pops in under 200 ms', async ({ page }) => {
  await page.emulateMedia({ reducedMotion: 'no-preference' });
  await open(page);
  const t = await page.evaluate(() => [getComputedStyle(document.documentElement).getPropertyValue('--dur-fast').trim(),
    getComputedStyle(document.documentElement).getPropertyValue('--dur').trim()]);
  expect(t).toEqual(['120ms', '180ms']);
  await go(page, 'view');
  await row(page, 'trackFocus').getByRole('switch').click({ force: true });
  const box = page.locator('.mbox');
  await expect(box).toBeVisible();
  expect(parseFloat(await css(box, 'animation-duration'))).toBeCloseTo(0.18, 2);
});

test('dragging an image or link out of the window is blocked', async ({ page }) => {
  await open(page);
  const prevented = await page.evaluate(() => {
    const e = new Event('dragstart', { cancelable: true, bubbles: true });
    document.querySelector('.side .it').dispatchEvent(e);
    return e.defaultPrevented;
  });
  expect(prevented).toBe(true);
});
