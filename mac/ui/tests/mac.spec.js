// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
// Mac-only parts of Settings: the macOS card on Preferences (permissions, the macOS Zoom shortcuts,
// Open at login, log detail, the file buttons) and the permission banner, against a mocked macStatus.
import { test, expect } from '@playwright/test';
import { mockHost, open, go, row, sent, live } from './mock.js';

const status = (page, k) => row(page, k).locator('.st');
const actions = async (page) => (await sent(page, 'macAction')).map((m) => [m.action, m.value]);
const push = (page, m) => page.evaluate((x) => window.__hostSend({ type: 'macStatus', ...x }), m);

test('all granted: the card says so, offers no fixes, and there is no banner', async ({ page }) => {
  await mockHost(page);
  await open(page);
  await expect(page.locator('.pb')).toHaveCount(0);
  await go(page, 'prefs');
  await expect(status(page, '__accessibility')).toHaveText('Allowed');
  await expect(status(page, '__screenRecording')).toHaveText('Allowed');
  await expect(status(page, '__zoomShortcuts')).toHaveText('Off');
  for (const k of ['__accessibility', '__screenRecording', '__zoomShortcuts'])
    await expect(row(page, k).locator('button.chip'), k).toHaveCount(0);
  await expect(row(page, '__loginItem').getByRole('switch')).not.toBeChecked();
});

test('missing permissions and shortcuts on: each row offers its fix', async ({ page }) => {
  await mockHost(page, { mac: { accessibility: false, screenRecording: false, zoomShortcuts: ['Scroll gesture', 'Keyboard shortcuts'] } });
  await open(page);
  await go(page, 'prefs');
  await expect(status(page, '__accessibility')).toHaveText('Not allowed');
  await expect(status(page, '__accessibility')).toHaveClass(/bad/);
  await expect(status(page, '__zoomShortcuts')).toHaveText('On');
  await expect(status(page, '__zoomShortcuts')).toHaveAttribute('title', 'Scroll gesture, Keyboard shortcuts');
  await row(page, '__accessibility').getByRole('button', { name: /Accessibility/ }).click();
  await row(page, '__screenRecording').getByRole('button', { name: /Screen Recording/ }).click();
  await row(page, '__zoomShortcuts').getByRole('button', { name: /Turn off/ }).click();
  expect(await actions(page)).toEqual([['openAccessibility', ''], ['openScreenRecording', ''], ['fixZoomShortcuts', '']]);
});

test('a pushed macStatus updates the card live', async ({ page }) => {
  await mockHost(page, { mac: { accessibility: false } });
  await open(page);
  await go(page, 'prefs');
  await expect(status(page, '__accessibility')).toHaveText('Not allowed');
  await push(page, { accessibility: true, screenRecording: true, zoomShortcuts: [], loginItem: true });
  await expect(status(page, '__accessibility')).toHaveText('Allowed');
  await expect(row(page, '__loginItem').getByRole('switch')).toBeChecked();
});

test('coming back to the window asks for the macOS status again', async ({ page }) => {
  await mockHost(page);
  await open(page);
  await expect.poll(async () => (await sent(page, 'getMacStatus')).length).toBe(1);
  await page.evaluate(() => window.dispatchEvent(new Event('focus')));
  await expect.poll(async () => (await sent(page, 'getMacStatus')).length).toBe(2);
});

test('Open at login is a switch that calls setLogin', async ({ page }) => {
  await mockHost(page);
  await open(page);
  await go(page, 'prefs');
  const sw = row(page, '__loginItem').getByRole('switch');
  await sw.check({ force: true });
  await expect(sw).toBeChecked();
  await sw.uncheck({ force: true });
  expect(await actions(page)).toEqual([['setLogin', '1'], ['setLogin', '0']]);
  expect(await sent(page, 'setConfig')).toEqual([]);
});

test('Log detail writes logLevel; the buttons reach the host', async ({ page }) => {
  await mockHost(page);
  await open(page);
  await go(page, 'prefs');
  const sel = row(page, 'logLevel').locator('select');
  await expect(sel.locator('option')).toHaveText(['Quiet', 'Sessions', 'Verbose', 'Every zoom step']);
  await sel.selectOption('2');
  await expect.poll(() => live(page, 'logLevel')).toBe('2');
  await row(page, '__showLog').getByRole('button').click();
  await row(page, '__openIni').getByRole('button').click();
  await row(page, '__diagnostics').getByRole('button').click();
  await row(page, '__rerunSetup').getByRole('button').click();
  expect(await actions(page)).toEqual([['showLog', ''], ['rerunSetup', '']]);
  expect(await sent(page, 'openIni')).toHaveLength(1);
  expect(await sent(page, 'exportDiagnostics')).toHaveLength(1);
  await expect(page.locator('[role=status]').filter({ hasText: 'Exporting diagnostics' })).toHaveCount(1);
});

test('no Windows-only rows: UIAccess, Restart Wind, game list, profiles', async ({ page }) => {
  await mockHost(page);
  await open(page);
  for (const g of ['hotkeys', 'zoom', 'view', 'prefs']) {
    await go(page, g);
    for (const t of ['UIAccess', 'Restart Wind', 'Share keys', 'Profile', 'Tray menu', 'taskbar'])
      await expect(page.locator('main').getByText(t), g + ' ' + t).toHaveCount(0);
  }
});

// ---- the permission banner ------------------------------------------------------------------------

test('the banner shows on every page while Accessibility is missing and opens its pane', async ({ page }) => {
  await mockHost(page, { mac: { accessibility: false } });
  await open(page);
  for (const g of ['hotkeys', 'zoom', 'view', 'prefs']) {
    await go(page, g);
    const pb = page.locator('main .pb');
    await expect(pb, g).toBeVisible();
    await expect(pb).toHaveAttribute('role', 'alert');
    await expect(pb.locator('.h')).toHaveText('Wind needs Accessibility');
  }
  // Under the page banner.
  const order = await page.locator('main').evaluate((m) => [...m.querySelectorAll('.banner, .pb')].map((e) => e.className.split(' ')[0]));
  expect(order).toEqual(['banner', 'pb']);
  await page.locator('main .pb').getByRole('button', { name: 'Open System Settings' }).click();
  expect(await actions(page)).toEqual([['openAccessibility', '']]);
});

test('Screen Recording matters only on the render engine', async ({ page }) => {
  await mockHost(page, { values: { engine: 'draw' }, mac: { screenRecording: false } });
  await open(page);
  await expect(page.locator('main .pb .h')).toHaveText('Wind needs Screen Recording');
  await expect(page.locator('main .pb')).toHaveAttribute('data-missing', 'screenRecording');
  // On the macOS zoom engine Screen Recording is not needed.
  await page.evaluate(() => window.__hostSend({ type: 'config', push: true, values: { ...window.__live, engine: 'zoom' } }));
  await expect(page.locator('main .pb')).toHaveCount(0);
});

test('two missing permissions: one banner naming both, the button opens Accessibility first', async ({ page }) => {
  await mockHost(page, { values: { engine: 'draw' }, mac: { accessibility: false, screenRecording: false } });
  await open(page);
  await expect(page.locator('main .pb .h')).toHaveText('Wind needs two permissions');
  await expect(page.locator('main .pb .d')).toContainText('Accessibility and Screen Recording');
  await page.locator('main .pb button').click();
  expect(await actions(page)).toEqual([['openAccessibility', '']]);
});

test('the banner goes away the moment macOS grants the permission', async ({ page }) => {
  await mockHost(page, { mac: { accessibility: false } });
  await open(page);
  await expect(page.locator('main .pb')).toBeVisible();
  await push(page, { accessibility: true, screenRecording: true, zoomShortcuts: [], loginItem: false });
  await expect(page.locator('main .pb')).toHaveCount(0);
});

test('the banner sits above search results too', async ({ page }) => {
  await mockHost(page, { mac: { accessibility: false } });
  await open(page);
  await page.getByLabel('Search settings').fill('zoom');
  await expect(page.locator('main .pb')).toBeVisible();
  await expect(page.locator('main .res')).toBeVisible();
});
