import { test, expect } from '@playwright/test';

test.beforeEach(async ({ page }) => {
  await page.addInitScript(() => {
    window.__sets = [];
    const listeners = new Set();
    window.chrome = { webview: {
      addEventListener: (_e, fn) => listeners.add(fn),
      postMessage: (msg) => {
        if (msg.type === 'getConfig')
          listeners.forEach(fn => fn({ data: { type: 'config', values: { zoomInSpeed: '1.2', smoothZoom: '0' } } }));
        if (msg.type === 'setConfig') window.__sets.push(msg);
      },
    }};
  });
});

test('renders settings sections and rows', async ({ page }) => {
  await page.goto('/');
  await expect(page.getByRole('button', { name: 'Zoom' })).toBeVisible();
  await expect(page.getByText('Zoom-in speed')).toBeVisible();
});

test('changes stage until Apply, then setConfig fires with the right key', async ({ page }) => {
  await page.goto('/');
  await page.getByText('Smooth zoom', { exact: true }).locator('xpath=../..').getByRole('checkbox').click();
  // Staged, not yet written.
  expect(await page.evaluate(() => window.__sets.length)).toBe(0);
  await page.getByRole('button', { name: 'Apply' }).click();
  const sets = await page.evaluate(() => window.__sets);
  expect(sets.some(s => s.key === 'smoothZoom' && s.value === '1')).toBeTruthy();
});
