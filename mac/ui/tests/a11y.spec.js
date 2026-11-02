// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
// Screen-reader and keyboard checks on the Mac settings window (issue #201 on Windows, kept for the Mac):
// every control is named by its row, descriptions are linked, focus is visible, dialogs trap focus,
// the macOS card and the banner speak sensibly.
import { test, expect } from '@playwright/test';
import { mockHost, open, go, row, advanced } from './mock.js';

const GROUPS = ['hotkeys', 'zoom', 'view', 'prefs', 'about'];

test.beforeEach(async ({ page }) => {
  await mockHost(page, { values: { maxLevel: '12', showAdvanced: '1', engine: 'zoom' },
    mac: { accessibility: false, zoomShortcuts: ['Scroll gesture'] } });
});

// Accessible name from aria-label, aria-labelledby, an associated label or content.
const unnamedIn = (page, root) => page.locator(root).evaluate((r) => {
  const out = [];
  for (const el of r.querySelectorAll('input, button, select, textarea, [role=switch], [role=radio], [role=combobox]')) {
    if (el.closest('[hidden]') || el.getAttribute('aria-hidden') === 'true') continue;
    const byIds = (a) => (el.getAttribute(a) || '').split(/\s+/).filter(Boolean)
      .map((id) => (document.getElementById(id) || {}).textContent || '').join(' ').trim();
    const name = (el.getAttribute('aria-label') || '') || byIds('aria-labelledby') ||
      (el.labels && el.labels[0] ? el.labels[0].textContent : '') || (el.textContent || '').trim() || (el.getAttribute('title') || '');
    if (!name.trim()) out.push(el.tagName.toLowerCase() + (el.type ? '[' + el.type + ']' : '') + ' in ' + (el.closest('[data-key]')?.getAttribute('data-key') || '?'));
  }
  return out;
});

test('every control on every page has an accessible name', async ({ page }) => {
  await open(page);
  for (const g of GROUPS) {
    await go(page, g);
    await expect(page.locator('.side .it.sel')).toHaveAttribute('data-g', g);
    expect(await unnamedIn(page, 'main'), g).toEqual([]);
  }
  expect(await unnamedIn(page, '.side')).toEqual([]);
});

test('a control is named by its own row label', async ({ page }) => {
  await open(page);
  await go(page, 'zoom');
  await expect(page.getByRole('slider', { name: /Max zoom/ })).toBeVisible();
  await expect(page.getByRole('combobox', { name: /Engine/ })).toBeVisible();
  await go(page, 'view');
  await expect(page.getByRole('switch', { name: /Follow the text cursor/ })).toBeVisible();
  await expect(page.getByRole('combobox', { name: /Pointer while zoomed/ })).toBeVisible();
  await go(page, 'prefs');
  await expect(page.getByRole('switch', { name: 'Open at login' })).toBeVisible();
  await expect(page.getByRole('button', { name: 'Open System Settings for Accessibility' })).toBeVisible();
  await expect(page.getByRole('button', { name: 'Turn off macOS Zoom shortcuts' })).toBeVisible();
  await expect(page.getByRole('button', { name: /Export diagnostics/ })).toBeVisible();
});

test('row descriptions are linked to their control, not orphaned', async ({ page }) => {
  await open(page);
  for (const g of GROUPS) {
    await go(page, g);
    const broken = await page.locator('main').evaluate((m) => [...m.querySelectorAll('[aria-describedby]')]
      .flatMap((el) => el.getAttribute('aria-describedby').split(/\s+/).filter(Boolean).filter((id) => !document.getElementById(id))));
    expect(broken, g).toEqual([]);
  }
});

test('sliders speak their unit instead of a bare number', async ({ page }) => {
  await open(page);
  await go(page, 'zoom');
  await expect(row(page, 'maxLevel').locator('input[type=range]')).toHaveAttribute('aria-valuetext', '12 times');
  await expect(row(page, 'zoomEaseOutMs').locator('input[type=range]')).toHaveAttribute('aria-valuetext', '115 ms');
});

test('the macOS status is readable as the fix button\'s description', async ({ page }) => {
  await open(page);
  await go(page, 'prefs');
  const btn = page.getByRole('button', { name: 'Open System Settings for Accessibility' });
  const desc = await btn.evaluate((el) => (el.getAttribute('aria-describedby') || '').split(/\s+/)
    .map((id) => (document.getElementById(id) || {}).textContent || '').join(' '));
  expect(desc).toContain('Not allowed');
});

test('the permission banner is an alert with a named button', async ({ page }) => {
  await open(page);
  await expect(page.getByRole('alert')).toContainText('Wind needs Accessibility');
  await expect(page.getByRole('alert').getByRole('button', { name: 'Open System Settings' })).toBeVisible();
});

test('focus is visible on the sidebar, a keybind box and the search field', async ({ page }) => {
  await open(page);
  await page.keyboard.press('Shift');   // any key: keyboard modality
  await page.keyboard.press('Alt+Tab');
  for (const sel of ['.side .it', 'main .kchg']) {
    await page.locator(sel).first().focus();
    const ring = await page.locator(sel).first().evaluate((el) => {
      const s = getComputedStyle(el);
      return (s.outlineStyle !== 'none' && parseFloat(s.outlineWidth) > 0) || s.boxShadow !== 'none';
    });
    expect(ring, sel).toBe(true);
  }
  const box = page.locator('.side .search');
  const before = await box.evaluate((el) => getComputedStyle(el).borderTopColor);
  await page.locator('.side .search input').focus();
  await expect(box).not.toHaveCSS('border-top-color', before);   // the field's frame lights up
});

test('a dialog takes focus, traps Tab, and gives focus back on close', async ({ page }) => {
  await page.addInitScript(() => { window.__refuse.push('trackFocus'); });
  await open(page);
  await go(page, 'view');
  const sw = row(page, 'trackFocus').getByRole('switch');
  await sw.focus();
  await page.keyboard.press('Space');
  const dlg = page.getByRole('dialog');
  await expect(dlg).toBeVisible();
  await expect(dlg).toHaveAttribute('aria-modal', 'true');
  await expect(dlg).toHaveAccessibleName("Couldn't save the setting");
  expect(await page.evaluate(() => !!document.activeElement.closest('[role=dialog]'))).toBe(true);
  for (let i = 0; i < 4; i++) {
    await page.keyboard.press('Tab');
    expect(await page.evaluate(() => !!document.activeElement.closest('[role=dialog]'))).toBe(true);
  }
  await page.keyboard.press('Escape');
  await expect(dlg).toHaveCount(0);
  await expect(sw).toBeFocused();
});

test('the sidebar is a navigation landmark that marks the current page', async ({ page }) => {
  await open(page);
  await expect(page.getByRole('navigation', { name: 'Settings sections' })).toBeVisible();
  await expect(page.locator('.side .it[aria-current=page]')).toHaveCount(1);
  await go(page, 'view');
  await expect(page.locator('.side .it[aria-current=page]')).toHaveAttribute('data-g', 'view');
  await expect(page.locator('main')).toHaveAttribute('aria-label', 'View');
});

test('the page has a lang, one h1, and hierarchical headings', async ({ page }) => {
  await open(page);
  await expect(page.locator('h1')).toHaveCount(1);
  expect(await page.evaluate(() => document.documentElement.lang)).toBe('en');
  const levels = await page.evaluate(() => [...document.querySelectorAll('h1,h2,h3,h4')].map((h) => +h.tagName[1]));
  for (let i = 1; i < levels.length; i++) expect(levels[i] - levels[i - 1]).toBeLessThanOrEqual(1);
});

test('decorative icons are hidden from the accessibility tree', async ({ page }) => {
  await open(page);
  const exposed = await page.locator('.wnd').evaluate((r) => [...r.querySelectorAll('svg')]
    .filter((s) => !s.closest('[aria-hidden=true]') && s.getAttribute('aria-hidden') !== 'true' && !s.getAttribute('aria-label')
      && !s.closest('button[aria-label]')).length);
  expect(exposed).toBe(0);
});

test('Tab escapes an armed keybind capture instead of binding Tab', async ({ page }) => {
  await open(page);
  const cap = row(page, 'zoomIn').locator('.kchg').first();
  await cap.focus();
  await page.keyboard.press('Enter');
  await expect(cap).toHaveAccessibleName(/Recording/);
  await page.keyboard.press('Tab');
  await expect(cap).toHaveAccessibleName(/Change/);
  expect(await page.evaluate(() => window.__msgs.filter((m) => m.type === 'setConfig'))).toEqual([]);
});

test('an armed keycap says so and carries its instructions as a description', async ({ page }) => {
  await open(page);
  const cap = row(page, 'zoomIn').locator('.kchg').first();
  await cap.click();
  await expect(cap).toHaveAccessibleName(/Recording/);
  const desc = await cap.evaluate((el) => (el.getAttribute('aria-describedby') || '').split(/\s+/)
    .map((id) => (document.getElementById(id) || {}).textContent || '').join(' '));
  expect(desc).toMatch(/Escape cancels/);
  await expect(row(page, 'zoomIn').locator('[role=status]')).toContainText(/Listening/);
});

test('the theme picker is a radio group with arrow-key navigation', async ({ page }) => {
  await open(page);
  await go(page, 'prefs');
  const group = row(page, 'uiPalette').getByRole('radiogroup');
  await expect(group.getByRole('radio')).toHaveCount(4);
  await group.getByRole('radio', { checked: true }).focus();
  await page.keyboard.press('ArrowRight');
  await expect(group.getByRole('radio', { checked: true })).toHaveAccessibleName('Ember');
  expect((await page.evaluate(() => window.__msgs.filter((m) => m.type === 'setConfig' && m.key === 'uiPalette'))).map((m) => m.value)).toEqual(['ember']);
});

test('the search box is a labelled search field; main is "Search results" while searching', async ({ page }) => {
  await open(page);
  const s = page.getByRole('searchbox', { name: 'Search settings' });
  await expect(s).toBeVisible();
  await s.fill('zoom');
  await expect(page.locator('main')).toHaveAttribute('aria-label', 'Search results');
});

test('the advanced switch is a named switch', async ({ page }) => {
  await open(page);
  await advanced(page, false);
  await expect(page.getByRole('switch', { name: 'Show advanced settings' })).not.toBeChecked();
});
