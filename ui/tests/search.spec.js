import { test, expect } from '@playwright/test';
import { readFileSync } from 'node:fs';
import { groups } from '../src/settings-schema.js';
import { search } from '../src/search/search.js';

test.beforeEach(async ({ page }) => {
  await page.addInitScript(() => {
    window.__skipSplash = true;
    const listeners = new Set();
    const send = (data) => listeners.forEach((fn) => fn({ data }));
    const live = { maxLevel: '12', model: 'hybrid', uiTheme: 'dark' };
    window.chrome = { webview: {
      addEventListener: (_e, fn) => listeners.add(fn),
      postMessage: (msg) => {
        if (msg.type === 'getConfig') send({ type: 'config', values: { ...live }, saved: { ...live },
          profiles: { names: ['Default'], active: 'Default' } });
        else if (msg.type === 'mpoState') send({ type: 'mpoState', disabled: false, bootKnown: true, atBoot: false });
      },
    } };
  });
});

const input = (page) => page.getByLabel('Search settings');

test('search is case-insensitive, word-prefix and covers label, description, caption and group', () => {
  expect(search(groups, '')).toEqual([]);
  const keys = (q) => search(groups, q).flatMap((g) => g.rows.map((r) => r.key));
  expect(keys('MAX ZO')).toContain('maxLevel');
  expect(keys('zoom-in speed')).toContain('zoomInSpeed');
  expect(keys('xyzzy')).toEqual([]);
  // Mid-word fragments do not match (word-prefix only).
  expect(keys('axzoom')).toEqual([]);
  // Group label and card caption reach their rows.
  expect(search(groups, 'moving').map((g) => g.id)).toContain('move');
  expect(keys('panning')).toContain('panSpeed');
  // The single pan row is found by both words (#303).
  expect(keys('pan')).toContain('__pan');
  expect(keys('arrow')).toContain('__pan');
  // Advanced rows are searchable, hidden-by-showIf ones included.
  expect(search(groups, 'engine').some((g) => g.id === 'advanced')).toBe(true);
});

test('search files contain no em-dash', () => {
  for (const f of ['../src/search/search.js', '../src/search/Results.svelte'])
    expect(readFileSync(new URL(f, import.meta.url), 'utf8')).not.toContain(String.fromCharCode(0x2014));
});

test('typing shows grouped results, Enter jumps to the first, Esc clears', async ({ page }) => {
  await page.goto('/');
  await expect(page.locator('h1')).toHaveText('Zoom');
  await input(page).fill('cursor');
  await expect(page.locator('.res')).toBeVisible();
  await expect(page.locator('.res .gl').first()).toBeVisible();
  const first = await page.locator('.res .hit').first().getAttribute('data-hit');
  await input(page).press('Enter');
  await expect(page.locator('.res')).toHaveCount(0);
  await expect(page.locator('[data-key="' + first + '"]')).toBeVisible();
  await input(page).fill('speed');
  await expect(page.locator('.res')).toBeVisible();
  await input(page).press('Escape');
  await expect(page.locator('.res')).toHaveCount(0);
  await expect(input(page)).toHaveValue('');
  await expect(page.locator('h1')).toBeVisible();
});

test('Ctrl+F focuses the search box, which shows a visible focus ring', async ({ page }) => {
  await page.goto('/');
  await expect(page.locator('h1')).toHaveText('Zoom');
  await page.locator('main').click({ position: { x: 5, y: 5 } });
  await page.keyboard.press('Control+f');
  await expect(input(page)).toBeFocused();
  await expect(page.locator('.search')).toHaveCSS('border-top-color', 'rgb(242, 242, 242)');   // after its short fade
});

test('keyboard only: Tab reaches a result and Enter opens it, Advanced rows reachable', async ({ page }) => {
  await page.goto('/');
  await input(page).fill('engine');
  await expect(page.locator('.res .gl', { hasText: 'Advanced' })).toBeVisible();
  await page.locator('.res .hit[data-hit="model"]').focus();
  await expect(page.locator('.res .hit[data-hit="model"]')).toBeFocused();
  await page.keyboard.press('Enter');
  await expect(page.locator('.res')).toHaveCount(0);
  await expect(page.locator('h1')).toHaveText('Advanced');
  await expect(page.locator('[data-key="model"]')).toBeVisible();
});

test('no matches says so', async ({ page }) => {
  await page.goto('/');
  await input(page).fill('qqqq');
  await expect(page.locator('.res .sum')).toContainText('No settings match');
});
