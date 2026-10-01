import { test, expect } from '@playwright/test';
import { readFileSync } from 'node:fs';
import { iconNames, iconSvg } from '../src/design/icons.js';

const css = (loc, prop) => loc.evaluate((el, p) => getComputedStyle(el).getPropertyValue(p), prop);

test('icon set covers the plan list and renders static svg', () => {
  for (const n of ['zoom', 'move', 'cursor', 'typing', 'colour', 'general', 'adv', 'about', 'search', 'auto', 'light', 'dark'])
    expect(iconNames).toContain(n);
  expect(iconSvg('zoom')).toMatch(/^<svg /);
  expect(iconSvg('nope')).toBe('');
});

test('tokens and design files contain no em-dash', () => {
  for (const f of ['../src/design/tokens.css', '../src/design/icons.js', '../src/shell/TitleBar.svelte',
    '../src/shell/Sidebar.svelte', '../src/shell/Banner.svelte', '../src/shell/Card.svelte', '../src/shell/SaveCapsule.svelte'])
    expect(readFileSync(new URL(f, import.meta.url), 'utf8')).not.toContain('—');
});

test('dark tokens and shell components render in isolation', async ({ page }) => {
  await page.goto('/preview.html');
  const app = page.locator('.wnd');
  expect(await css(app, '--card')).toBe('#1b1b1b');
  expect(await css(app, '--hlGrey')).toBe('#313131');
  expect(await css(page.locator('.card'), 'border-top-left-radius')).toBe('10px');
  expect(await css(page.locator('.card'), 'background-color')).toBe('rgb(27, 27, 27)');
  expect(await css(page.locator('.it').first(), 'border-top-left-radius')).toBe('8px');
  expect(await css(page.locator('.it').first(), 'height')).toBe('36px');
  expect(await css(page.locator('.it.sel'), 'background-color')).toBe('rgb(49, 49, 49)');
  await expect(page.locator('.banner h1')).toHaveText('Zoom');
  await expect(page.locator('.banner .bdesc')).toContainText('Keys, limits');
  await expect(page.locator('.capsule b')).toHaveText('2 unsaved changes');
  await expect(page.locator('.side .n')).toHaveText('0.15.4');
  await expect(page.locator('.tb .brand')).toContainText('Wind Settings');
});

test('light theme tokens apply', async ({ page }) => {
  await page.goto('/preview.html?theme=light');
  expect(await css(page.locator('.card'), 'background-color')).toBe('rgb(255, 255, 255)');
  expect(await css(page.locator('.side'), 'background-color')).toBe('rgb(243, 243, 244)');
  expect(await css(page.locator('.it.sel'), 'background-color')).toBe('rgb(228, 228, 228)');
  expect(await css(page.locator('.banner'), 'background-color')).toBe('rgb(246, 246, 247)');
});

test('callbacks fire and the capsule hides at zero', async ({ page }) => {
  await page.goto('/preview.html');
  await page.getByRole('button', { name: 'Cursor' }).click();
  await expect(page.locator('.it.sel')).toContainText('Cursor');
  await page.getByRole('button', { name: 'Save' }).click();
  await page.getByRole('button', { name: 'Discard' }).click();
  await page.getByRole('button', { name: 'Minimize' }).click();
  await page.getByRole('button', { name: 'Close' }).click();
  await page.getByRole('button', { name: /^Theme:/ }).click();
  await page.getByPlaceholder('Search settings').fill('zoom');
  expect(await page.evaluate(() => window.__calls)).toEqual([
    ['select', 'cursor'], 'save', 'discard', 'minimize', 'close', ['theme', 'light'], ['search', 'zoom'],
  ]);
  await page.goto('/preview.html?count=0');
  await expect(page.locator('.capsule')).toHaveCount(0);
});
