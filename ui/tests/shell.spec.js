// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
// The window shell on the Mac: no title bar row (macOS draws the traffic lights), the sidebar keeps
// their corner free, always dark, Mac system fonts.
import { test, expect } from '@playwright/test';
import { readFileSync, readdirSync, statSync } from 'node:fs';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { iconNames, iconSvg } from '../src/design/icons.js';
import { mockHost, open, go, css } from './mock.js';

test.beforeEach(async ({ page }) => { await mockHost(page); });

test('icon set covers the pages and renders static svg', () => {
  for (const n of ['hotkeys', 'zoom', 'view', 'general', 'about', 'search'])
    expect(iconNames).toContain(n);
  expect(iconSvg('zoom')).toMatch(/^<svg /);
  expect(iconSvg('nope')).toBe('');
});

test('no source file has an em-dash', () => {
  const src = fileURLToPath(new URL('../src', import.meta.url));
  const walk = (d) => readdirSync(d).flatMap((f) => statSync(join(d, f)).isDirectory() ? walk(join(d, f)) : [join(d, f)]);
  for (const f of walk(src).filter((f) => /\.(js|svelte|css)$/.test(f)))
    expect(readFileSync(f, 'utf8'), f).not.toContain('—');
});

test('no Windows title bar, save capsule or profile picker', async ({ page }) => {
  await open(page);
  await expect(page.locator('.tb')).toHaveCount(0);
  await expect(page.getByRole('button', { name: /^(Minimize|Maximize|Close)$/ })).toHaveCount(0);
  await expect(page.locator('.capsule')).toHaveCount(0);
  await expect(page.getByRole('button', { name: /^(Save|Discard)$/ })).toHaveCount(0);
  await go(page, 'prefs');
  await expect(page.getByText('Profile', { exact: true })).toHaveCount(0);
});

test('the sidebar leaves the top-left corner free for the traffic lights', async ({ page }) => {
  await open(page);
  const box = await page.locator('.side .search').boundingBox();
  expect(box.y).toBeGreaterThanOrEqual(40);    // the traffic lights sit in the top ~28px, at y 0..40
  expect(await css(page.locator('.side'), 'padding-top')).toBe('44px');
});

test('dark tokens: card, selected row and radii', async ({ page }) => {
  await open(page);
  expect(await css(page.locator('.card').first(), 'border-top-left-radius')).toBe('10px');
  expect(await css(page.locator('.card').first(), 'background-color')).toBe('rgb(8, 8, 8)');
  expect(await css(page.locator('.it').first(), 'height')).toBe('36px');
  expect(await css(page.locator('.it.sel'), 'background-color')).toBe('rgb(49, 49, 49)');
});

test('text uses the Mac system font and mono uses ui-monospace', async ({ page }) => {
  await open(page);
  const s = await css(page.locator('.app'), '--s');
  const m = await css(page.locator('.app'), '--m');
  expect(s).toMatch(/-apple-system|system-ui/);
  expect(s).not.toMatch(/Segoe/);
  expect(m).toMatch(/ui-monospace/);
  expect(m).not.toMatch(/Cascadia/);
  expect(await css(page.locator('.row .label').first(), 'font-family')).toMatch(/-apple-system|system-ui/);
});

test('a theme param or a light uiTheme never makes it light', async ({ page }) => {
  await page.addInitScript(() => { window.__live.uiTheme = 'light'; });
  await open(page, '/?theme=light');
  expect(await css(page.locator('.card').first(), 'background-color')).toBe('rgb(8, 8, 8)');
  expect(await css(page.locator('.side'), 'background-color')).toBe('rgb(0, 0, 0)');
});

test('the page tells the host when it has painted', async ({ page }) => {
  await open(page);
  await expect.poll(() => page.evaluate(() => window.__msgs.some((m) => m.type === 'ready'))).toBe(true);
});

test('the controls preview renders', async ({ page }) => {
  const errors = [];
  page.on('pageerror', (e) => errors.push(e.message));
  await page.goto('/controls.html');
  await expect(page.locator('#app')).not.toBeEmpty();
  expect(errors).toEqual([]);
});
