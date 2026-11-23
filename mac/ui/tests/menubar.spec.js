// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
// Settings "Menu bar" page: the menu bar icon and Performance switches, the Sliders and Toggles lists
// (ui-shared/src/tray/ItemListsPage.svelte) with check, drag and keyboard reorder, the four-slider cap,
// and the menu* config keys written at once through the mock host. The parse rules themselves are
// menu-layout.nodetest.mjs (shared with MenuLayout.swift).
import { test, expect } from '@playwright/test';
import { mockHost, open, go, live } from './mock.js';

test.beforeEach(async ({ page }) => { await mockHost(page, { values: { maxLevel: '12' } }); });

const show = async (page) => { await open(page); await go(page, 'menubar'); };
const rowsOf = (page, list) => page.locator('#cap-' + list).locator('xpath=ancestor::div[contains(@class,"tcap")]/following-sibling::div[contains(@class,"tlist")][1]').locator('.trow');
const names = (page, list) => rowsOf(page, list).locator('.k').allTextContents();
const chk = (page, list, name) => rowsOf(page, list).filter({ hasText: name }).locator('.chk');
const SLIDER_NAMES = ['Warmth', 'Brightness', 'Saturation', 'Max zoom', 'Zoom-in speed', 'Zoom-out speed', 'Arrow key speed', 'Release glide', 'Pointer weight'];
const TOGGLE_NAMES = ['Follow the text cursor', 'Follow keyboard focus', 'Keep cursor centred', 'Engine'];

test('page: below the divider, the two switches, then both lists with the defaults', async ({ page }) => {
  await show(page);
  await expect(page.locator('h1')).toHaveText('Menu bar');
  await expect(page.locator('.side nav.bottom .it')).toHaveText([/Preferences/, /Menu bar/, /About/]);
  await expect(page.locator('main .row')).toHaveCount(2);
  expect(await page.locator('main .row').evaluateAll((rs) => rs.map((r) => r.getAttribute('data-key')))).toEqual(['showMenuBarIcon', 'menuPerf']);
  await expect(page.getByRole('switch', { name: 'Show menu bar icon' })).toBeChecked();
  await expect(page.getByRole('switch', { name: 'Performance in the panel' })).toBeChecked();
  expect(await names(page, 'sliders')).toEqual(SLIDER_NAMES);
  expect(await names(page, 'toggles')).toEqual(TOGGLE_NAMES);
  await expect(page.locator('[data-cnt="sliders"]')).toHaveText('2 of 4');
  await expect(page.locator('[data-cnt="toggles"]')).toHaveText('4 on');
  await expect(chk(page, 'sliders', 'Warmth')).toHaveAttribute('aria-checked', 'true');
  await expect(chk(page, 'sliders', 'Brightness')).toHaveAttribute('aria-checked', 'true');
  await expect(chk(page, 'sliders', 'Max zoom')).toHaveAttribute('aria-checked', 'false');
  await expect(page.getByRole('checkbox', { name: 'Show Pointer weight in the menu bar' })).toBeVisible();
  expect(await page.locator('.tic svg').count()).toBe(13);
});

test('the switches write showMenuBarIcon and menuPerf', async ({ page }) => {
  await show(page);
  await page.getByRole('switch', { name: 'Performance in the panel' }).uncheck({ force: true });
  expect(await live(page, 'menuPerf')).toBe('0');
  await page.getByRole('switch', { name: 'Show menu bar icon' }).uncheck({ force: true });
  expect(await live(page, 'showMenuBarIcon')).toBe('0');
});

test('check and uncheck write the enabled list and the full order at once', async ({ page }) => {
  await show(page);
  await chk(page, 'sliders', 'Pointer weight').click();
  expect(await live(page, 'menuSliders')).toBe('warmthPct,dimPct,panSmoothMs');
  expect(await live(page, 'menuSliderOrder')).toBe('warmthPct,dimPct,satPct,maxLevel,zoomInSpeed,zoomOutSpeed,panSpeed,zoomEaseOutMs,panSmoothMs');
  await chk(page, 'sliders', 'Warmth').click();
  expect(await live(page, 'menuSliders')).toBe('dimPct,panSmoothMs');
  await rowsOf(page, 'toggles').filter({ hasText: 'Follow keyboard focus' }).locator('.k').click();
  expect(await live(page, 'menuToggles')).toBe('trackCaret,keepEdges,engine');
  expect(await live(page, 'menuToggleOrder')).toBe('trackCaret,trackFocus,keepEdges,engine');
  await expect(page.locator('[data-cnt="toggles"]')).toHaveText('3 on');
});

test('slider cap: at four the unchecked boxes are disabled; toggles are not capped', async ({ page }) => {
  await show(page);
  await chk(page, 'sliders', 'Max zoom').click();
  await chk(page, 'sliders', 'Arrow key speed').click();
  await expect(page.locator('[data-cnt="sliders"]')).toHaveText('4 of 4 full');
  await expect(chk(page, 'sliders', 'Release glide')).toBeDisabled();
  const before = await live(page, 'menuSliders');
  await rowsOf(page, 'sliders').filter({ hasText: 'Release glide' }).locator('.k').click();
  expect(await live(page, 'menuSliders')).toBe(before);
  await chk(page, 'sliders', 'Warmth').click();
  await expect(chk(page, 'sliders', 'Release glide')).toBeEnabled();
});

test('keyboard reorder writes the order, both still on (the enabled list follows the new order)', async ({ page }) => {
  await show(page);
  const grip = rowsOf(page, 'sliders').filter({ hasText: 'Warmth' }).locator('.grip');
  await grip.focus();
  await page.keyboard.press('Space');
  await page.keyboard.press('ArrowDown');
  await page.keyboard.press('Space');
  await expect.poll(async () => (await names(page, 'sliders')).slice(0, 2)).toEqual(['Brightness', 'Warmth']);
  expect((await live(page, 'menuSliderOrder')).split(',').slice(0, 2)).toEqual(['dimPct', 'warmthPct']);
  expect(await live(page, 'menuSliders')).toBe('dimPct,warmthPct');
});

test('drag reorders the Toggles list', async ({ page }) => {
  await page.setViewportSize({ width: 1120, height: 1100 });
  await show(page);
  const rows = rowsOf(page, 'toggles');
  await rows.last().scrollIntoViewIfNeeded();
  const a = await rows.filter({ hasText: 'Keep cursor centred' }).locator('.tic').boundingBox();
  const b = await rows.filter({ hasText: 'Follow the text cursor' }).locator('.tic').boundingBox();
  await page.mouse.move(a.x + a.width / 2, a.y + a.height / 2);
  await page.mouse.down();
  await page.mouse.move(a.x + a.width / 2, a.y + a.height / 2 - 8, { steps: 3 });
  await page.mouse.move(b.x + b.width / 2, b.y + b.height / 2 - 6, { steps: 12 });
  await page.mouse.up();
  await expect.poll(() => names(page, 'toggles')).toEqual(['Keep cursor centred', 'Follow the text cursor', 'Follow keyboard focus', 'Engine']);
  expect(await live(page, 'menuToggleOrder')).toBe('keepEdges,trackCaret,trackFocus,engine');
});

test('the saved lists load back from config.json, as the host sends them', async ({ page }) => {
  // The host sends every key, lists comma-joined and an empty order as '' (SettingsWeb.values).
  await mockHost(page, { values: { menuPerf: '0', menuSliders: 'zoomOutSpeed,bogus', menuSliderOrder: 'zoomOutSpeed,dimPct',
    menuToggles: '', menuToggleOrder: '' } });
  await show(page);
  await expect(page.getByRole('switch', { name: 'Performance in the panel' })).not.toBeChecked();
  expect((await names(page, 'sliders')).slice(0, 3)).toEqual(['Zoom-out speed', 'Brightness', 'Warmth']);
  await expect(page.locator('[data-cnt="sliders"]')).toHaveText('1 of 4');
  await expect(page.locator('[data-cnt="toggles"]')).toHaveText('0 on');
});

test('search finds the menu bar rows on the Menu bar page', async ({ page }) => {
  await open(page);
  await page.locator('.side .search input').fill('menu bar icon');
  await expect(page.locator('.res .hit[data-hit="showMenuBarIcon"]')).toBeVisible();
  await expect(page.locator('.res .hit[data-hit="showMenuBarIcon"] .tab')).toHaveText('Menu bar');
});
