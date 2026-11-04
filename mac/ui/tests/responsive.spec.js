// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
// Window sizes: nothing overflows from the Mac window's minimum (700x480) up to a wide display, the
// content is capped on wide windows, and narrow windows move the sidebar above the page while keeping
// the traffic lights' corner free.
import { test, expect } from '@playwright/test';
import { mockHost, open, go } from './mock.js';

const GROUPS = ['hotkeys', 'zoom', 'view', 'prefs', 'about'];
const SIZES = [[700, 480], [820, 560], [1040, 720], [1440, 900], [2560, 1440]];

test.beforeEach(async ({ page }) => {
  await mockHost(page, { values: { showAdvanced: '1', engine: 'zoom' },
    mac: { accessibility: false, screenRecording: false, zoomShortcuts: ['Scroll gesture', 'Keyboard shortcuts'] } });
});

const problems = (page) => page.evaluate(() => {
  const out = [];
  const doc = document.documentElement;
  if (doc.scrollWidth > doc.clientWidth) out.push('page scrolls horizontally');
  const main = document.querySelector('main');
  if (main && main.scrollWidth > main.clientWidth) out.push('main scrolls horizontally');
  for (const card of document.querySelectorAll('.card, .pb')) {
    const cr = card.getBoundingClientRect();
    for (const el of card.querySelectorAll('.row .meta, .row .ctl, .row .ctl *, button')) {
      const r = el.getBoundingClientRect();
      if (r.width === 0 || r.height === 0) continue;
      if (r.right > cr.right + 1 || r.left < cr.left - 1) out.push((el.className || el.tagName) + ' leaves its card');
    }
  }
  return out;
});

for (const [w, h] of SIZES) {
  test(`no overflow at ${w}x${h}`, async ({ page }) => {
    await page.setViewportSize({ width: w, height: h });
    await open(page);
    for (const g of GROUPS) {
      await go(page, g);
      await page.waitForTimeout(150);
      expect(await problems(page), g).toEqual([]);
    }
    await page.locator('.side .search input').fill('zoom');
    await page.waitForTimeout(150);
    expect(await problems(page), 'search').toEqual([]);
  });
}

test('wide window caps the content width', async ({ page }) => {
  await page.setViewportSize({ width: 2560, height: 1440 });
  await open(page);
  await go(page, 'zoom');
  const w = await page.locator('.card').first().evaluate((el) => el.getBoundingClientRect().width);
  expect(w).toBeLessThanOrEqual(960);
});

test('narrow window moves the sidebar above the page and keeps the traffic lights clear', async ({ page }) => {
  await page.setViewportSize({ width: 420, height: 700 });
  await open(page);
  const side = await page.locator('.side').boundingBox();
  expect(side.width).toBeGreaterThan(380);
  const search = await page.locator('.side .search').boundingBox();
  expect(search.x).toBeGreaterThanOrEqual(78);   // the traffic lights take the top-left 78px
});

for (const [w, h] of [[700, 480], [900, 600]]) {
  test(`onboarding fits ${w}x${h}`, async ({ page }) => {
    await page.setViewportSize({ width: w, height: h });
    await page.goto('/?mode=onboard');
    await expect(page.getByRole('button', { name: 'Get started' })).toBeVisible();
    for (let i = 0; i < 3; i++) {                    // Transform (the default) has no Screen Recording step
      await page.locator('.wizfoot .btn.primary:not(:disabled), .wizfoot .btn:has-text("Skip this step")').first().click();
      await page.waitForTimeout(200);
      const fits = await page.evaluate(() => document.documentElement.scrollWidth <= document.documentElement.clientWidth);
      expect(fits).toBe(true);
      await expect(page.locator('.wizfoot .btn.primary')).toBeInViewport();
    }
  });
}

test('subtexts stay on one line at the default window size', async ({ page }) => {
  await open(page);
  for (const g of GROUPS) {
    await go(page, g);
    await page.waitForTimeout(150);
    const wrapped = await page.evaluate(() => [...document.querySelectorAll('main .desc, main .bdesc')].filter((el) => {
      const r = el.getBoundingClientRect();
      if (r.width === 0 || r.height === 0) return false;
      const lh = parseFloat(getComputedStyle(el).lineHeight) || parseFloat(getComputedStyle(el).fontSize) * 1.3;
      return r.height > lh * 1.5;
    }).map((el) => el.textContent));
    expect(wrapped, g).toEqual([]);
  }
});
