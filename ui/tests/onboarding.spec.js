// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
// Mac first-run setup (?mode=onboard) against the mock host: the steps follow the engine and the
// macOS status the host injected, the permission steps follow pushed macStatus, the zoom keys write
// the Mac bind lists, and finishing or skipping writes onboarded and opens Settings.
import { test, expect } from '@playwright/test';
import { mockHost, sent, wrote, live } from './mock.js';

const NONE = { accessibility: false, screenRecording: false, zoomShortcuts: [], loginItem: false };
const ALL = { accessibility: true, screenRecording: true, zoomShortcuts: [], loginItem: false };
const primary = (page) => page.locator('.wizfoot .btn.primary');
const step = (page) => page.locator('.step');
const grant = (page, m) => page.evaluate((x) => window.__hostSend({ type: 'macStatus', ...x }), m);
const actions = async (page) => (await sent(page, 'macAction')).map((m) => m.action);

test('steps on the render engine: welcome, Accessibility, Screen Recording, keys, done', async ({ page }) => {
  await mockHost(page, { init: true, mac: NONE });
  await page.goto('/?mode=onboard');
  await expect(page.locator('h1')).toHaveText('Welcome to Wind');
  await expect(page.locator('.wizdots i')).toHaveCount(5);
  await expect(primary(page)).toHaveText('Get started');
  await primary(page).click();

  // Accessibility: Continue waits for macOS.
  await expect(step(page)).toHaveAttribute('data-step', 'access');
  await expect(primary(page)).toBeDisabled();
  await expect(page.locator('.step .st')).toHaveText('Not allowed yet');
  await page.locator('.step').getByRole('button', { name: 'Open System Settings' }).click();
  expect(await actions(page)).toEqual(['openAccessibility']);
  await grant(page, { ...NONE, accessibility: true });
  await expect(page.locator('.step .st')).toHaveText('Allowed');
  await expect(page.locator('.step').getByRole('button', { name: 'Open System Settings' })).toHaveCount(0);
  await expect(page.getByRole('button', { name: 'Skip this step' })).toHaveCount(0);
  await primary(page).click();

  // Screen Recording.
  await expect(step(page)).toHaveAttribute('data-step', 'screen');
  await expect(primary(page)).toBeDisabled();
  await page.locator('.step').getByRole('button', { name: 'Open System Settings' }).click();
  expect(await actions(page)).toEqual(['openAccessibility', 'openScreenRecording']);
  await grant(page, ALL);
  await expect(primary(page)).toBeEnabled();
  await primary(page).click();

  await expect(step(page)).toHaveAttribute('data-step', 'keys');
  await primary(page).click();
  await expect(step(page)).toHaveAttribute('data-step', 'done');
  await expect(primary(page)).toHaveText('Open Settings');
  expect(await wrote(page, 'onboarded')).toBeUndefined();
  await primary(page).click();
  await expect.poll(() => live(page, 'onboarded')).toBe('1');
  await expect(page.locator('.side')).toBeVisible();          // Settings, in place
  await expect(page.locator('main h1')).toHaveText('Hotkeys');
});

test('on the macOS zoom engine there is no Screen Recording step', async ({ page }) => {
  await mockHost(page, { init: true, values: { engine: 'zoom' }, mac: ALL });
  await page.goto('/?mode=onboard');
  await expect(page.locator('.wizdots i')).toHaveCount(4);
  await primary(page).click();
  await expect(step(page)).toHaveAttribute('data-step', 'access');
  await expect(primary(page)).toBeEnabled();                  // already allowed
  await primary(page).click();
  await expect(step(page)).toHaveAttribute('data-step', 'keys');
});

test('the macOS Zoom shortcuts step shows only when some were on, and turns them off', async ({ page }) => {
  await mockHost(page, { init: true, mac: { ...ALL, zoomShortcuts: ['Scroll gesture'] } });
  await page.goto('/?mode=onboard');
  await expect(page.locator('.wizdots i')).toHaveCount(6);
  for (let i = 0; i < 3; i++) await primary(page).click();
  await expect(step(page)).toHaveAttribute('data-step', 'zoom');
  await expect(page.locator('.step .st')).toHaveText('On: Scroll gesture');
  await page.locator('.step').getByRole('button', { name: 'Turn off' }).click();
  expect(await actions(page)).toEqual(['fixZoomShortcuts']);
  await grant(page, ALL);
  await expect(page.locator('.step .st')).toHaveText('All off');
  await expect(page.locator('.wizdots i')).toHaveCount(6);    // the step stays while setup is open
});

test('Skip this step moves on without the permission; Back goes back', async ({ page }) => {
  await mockHost(page, { init: true, mac: NONE });
  await page.goto('/?mode=onboard');
  await primary(page).click();
  await page.getByRole('button', { name: 'Skip this step' }).click();
  await expect(step(page)).toHaveAttribute('data-step', 'screen');
  await page.getByRole('button', { name: 'Back' }).click();
  await expect(step(page)).toHaveAttribute('data-step', 'access');
});

test('Skip setup writes onboarded and opens Settings', async ({ page }) => {
  await mockHost(page, { init: true, mac: NONE });
  await page.goto('/?mode=onboard');
  await page.getByRole('button', { name: 'Skip setup' }).click();
  await expect.poll(() => live(page, 'onboarded')).toBe('1');
  await expect(page.locator('main h1')).toHaveText('Hotkeys');
  await expect(page.locator('main .pb')).toBeVisible();     // the permissions are still missing
});

test('the zoom keys step shows the Mac binds and records a new one', async ({ page }) => {
  await mockHost(page, { init: true, mac: ALL });
  await page.goto('/?mode=onboard');
  for (let i = 0; i < 3; i++) await primary(page).click();
  await expect(step(page)).toHaveAttribute('data-step', 'keys');
  const zin = page.locator('#ob-in-v');
  await expect(zin).toContainText('Mouse 4');
  await zin.click();
  await expect(zin).toHaveClass(/armed/);
  await page.keyboard.press('Control+Alt+KeyK');
  await expect.poll(() => wrote(page, 'zoomIn')).toBe('ctrl+opt+k');
  await expect(zin).not.toHaveClass(/armed/);
  // A refused combination stays armed and says why.
  const zout = page.locator('#ob-out-v');
  await zout.click();
  await page.keyboard.press('KeyK');
  await expect(page.locator('.refusal')).toContainText("alone would stop you typing");
  expect(await wrote(page, 'zoomOut')).toBeUndefined();
  await page.keyboard.press('Escape');
  // Recording paused Wind's hotkeys and resumed them.
  const caps = (await sent(page, 'captureBinds')).map((m) => m.on);
  expect(caps).toContain('1');
  expect(caps.at(-1)).toBe('0');
});

test('Settings mode does not show onboarding', async ({ page }) => {
  await mockHost(page);
  await page.goto('/');
  await expect(page.locator('main h1')).toHaveText('Hotkeys');
  await expect(page.locator('.wizfoot')).toHaveCount(0);
});

test('setup uses the chosen theme, and Wind grey on a first run', async ({ page }) => {
  await mockHost(page, { init: true, values: { uiPalette: 'ember' } });
  await page.goto('/?mode=onboard');
  await expect(page.locator('.win')).toHaveAttribute('data-palette', 'ember');
  const p2 = await page.context().newPage();
  await mockHost(p2, { init: true });
  await p2.goto('/?mode=onboard');
  await expect(p2.locator('.win')).toHaveAttribute('data-palette', 'grey');
});

test('the step is announced to screen readers', async ({ page }) => {
  await mockHost(page, { init: true, mac: ALL });
  await page.goto('/?mode=onboard');
  await expect(page.locator('.sr-only[role=status]')).toHaveText(/Step 1 of 5: Welcome to Wind/);
  await primary(page).click();
  await expect(page.locator('.sr-only[role=status]')).toHaveText(/Step 2 of 5: Allow Accessibility/);
});
