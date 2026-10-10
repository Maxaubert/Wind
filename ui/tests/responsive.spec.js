// Responsive layout: no horizontal overflow of the page and no control sticking out of its card at
// window sizes from a narrow strip to a very wide monitor, on every page, search and onboarding.
import { test, expect } from '@playwright/test';

const SIZES = [[420, 700], [560, 800], [820, 560], [900, 360], [1000, 1400], [1280, 800], [2560, 1440]];

const boot = (extra = {}) => async (page) => {
  await page.addInitScript((extra) => {
    const listeners = new Set();
    const live = { maxLevel: '12', zoomInVk: '33', zoomInButton: '2', zoomOutVk: '114', cursorLockVk: '113',
      model: 'hybrid', uiTheme: 'dark', showAdvanced: '1', ...extra };
    window.chrome = { webview: {
      addEventListener: (_e, fn) => listeners.add(fn),
      postMessage: (msg) => {
        if (msg.type === 'getConfig') listeners.forEach((fn) => fn({ data: { type: 'config', values: { ...live }, saved: { ...live },
          profiles: { names: ['Default', 'Gaming'], active: 'Default' } } }));
      },
    } };
  }, extra);
};

// Returns problems: page-level horizontal overflow, and any control or text wider than its card.
const problems = (page) => page.evaluate(() => {
  const out = [];
  const de = document.documentElement;
  if (de.scrollWidth > de.clientWidth) out.push('document scrolls horizontally');
  const main = document.querySelector('main.main');
  if (main && main.scrollWidth > main.clientWidth) out.push('main scrolls horizontally');
  for (const card of document.querySelectorAll('.card')) {
    const cr = card.getBoundingClientRect();
    for (const el of card.querySelectorAll('.row .meta, .row .ctl, .row .ctl *')) {
      const r = el.getBoundingClientRect();
      if (r.width === 0 || r.height === 0) continue;
      if (r.right > cr.right + 1 || r.left < cr.left - 1) out.push((el.className || el.tagName) + ' leaves its card');
    }
  }
  return out;
});

for (const [w, h] of SIZES) {
  test(`no overflow at ${w}x${h}`, async ({ page }) => {
    await boot()(page);
    await page.setViewportSize({ width: w, height: h });
    await page.goto('/');
    await expect(page.locator('h1')).toHaveText('Hotkeys');
    for (const g of ['hotkeys', 'zoom', 'view', 'prefs', 'tray', 'about']) {
      await page.locator('.side .it[data-g="' + g + '"]').click();
      await page.waitForTimeout(150);
      expect(await problems(page), g).toEqual([]);
    }
    await page.locator('.side .search input').fill('zoom');
    await page.waitForTimeout(150);
    expect(await problems(page), 'search').toEqual([]);
  });
}

test('wide window caps the content width', async ({ page }) => {
  await boot()(page);
  await page.setViewportSize({ width: 2560, height: 1440 });
  await page.goto('/');
  await page.locator('.side .it[data-g="zoom"]').click();
  const w = await page.locator('.card').first().evaluate((el) => el.getBoundingClientRect().width);
  expect(w).toBeLessThanOrEqual(960);
});

test('narrow window moves the sidebar above the page', async ({ page }) => {
  await boot()(page);
  await page.setViewportSize({ width: 420, height: 700 });
  await page.goto('/');
  const side = await page.locator('.side').evaluate((el) => el.getBoundingClientRect().width);
  expect(side).toBeGreaterThan(380);
});

for (const [w, h] of [[420, 700], [900, 360]]) {
  test(`onboarding fits ${w}x${h}`, async ({ page }) => {
    await boot({ uiTheme: 'auto' })(page);
    await page.setViewportSize({ width: w, height: h });
    await page.goto('/?mode=onboard');
    await expect(page.getByRole('button', { name: 'Get started' })).toBeVisible();
    for (const name of ['Get started', 'Next']) {
      await page.getByRole('button', { name }).click();
      await page.waitForTimeout(300);
      const fits = await page.evaluate(() => document.documentElement.scrollWidth <= document.documentElement.clientWidth);
      expect(fits).toBe(true);
    }
  });
}

for (const [w, h] of [[820, 560], [900, 600]]) {
  test(`subtexts stay on one line at ${w}x${h}`, async ({ page }) => {
    await boot()(page);
    await page.setViewportSize({ width: w, height: h });
    await page.goto('/');
    for (const g of ['hotkeys', 'zoom', 'view', 'prefs', 'tray', 'about']) {
      await page.locator('.side .it[data-g="' + g + '"]').click();
      await page.waitForTimeout(150);
      const wrapped = await page.evaluate(() => [...document.querySelectorAll('.desc, .bdesc, .thint')].filter((el) => {
        const r = el.getBoundingClientRect();
        if (r.width === 0 || r.height === 0) return false;
        const lh = parseFloat(getComputedStyle(el).lineHeight) || parseFloat(getComputedStyle(el).fontSize) * 1.3;
        return r.height > lh * 1.5;
      }).map((el) => el.textContent));
      expect(wrapped, g).toEqual([]);
    }
  });
}
