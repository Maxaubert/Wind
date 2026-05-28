import { test, expect } from '@playwright/test';

test.beforeEach(async ({ page }) => {
  await page.addInitScript(() => {
    window.__skipSplash = true;
    window.__sets = [];
    const listeners = new Set();
    window.chrome = { webview: {
      addEventListener: (_e, fn) => listeners.add(fn),
      postMessage: (msg) => {
        if (msg.type === 'getConfig')
          listeners.forEach(fn => fn({ data: { type: 'config', values: { uiTheme: 'auto' } } }));
        if (msg.type === 'setConfig') window.__sets.push(msg);
      },
    }};
  });
});

test('onboarding walks 3 steps, applies keys on advance, sets onboarded', async ({ page }) => {
  await page.goto('/?mode=onboard');
  await expect(page.getByRole('heading', { name: 'Welcome to Wind' })).toBeVisible();
  await page.getByRole('button', { name: 'Get started' }).click();
  await expect(page.getByRole('heading', { name: 'Set your zoom keys' })).toBeVisible();
  // Capture a key for Zoom in (writes setConfig live; no Apply step in onboarding).
  await page.getByText('Zoom in', { exact: true }).locator('xpath=../..').getByRole('button').click();
  await page.keyboard.press('F2'); // keyCode 113
  expect(await page.evaluate(() => window.__sets.some(s => s.key === 'zoomInVk' && s.value === '113'))).toBeTruthy();
  await page.getByRole('button', { name: 'Next' }).click();
  await expect(page.getByRole('heading', { name: "You're all set" })).toBeVisible();
  await page.getByRole('button', { name: 'Open Settings' }).click();
  expect(await page.evaluate(() => window.__sets.some(s => s.key === 'onboarded' && s.value === '1'))).toBeTruthy();
});

test('settings mode does not show onboarding', async ({ page }) => {
  await page.goto('/');
  await expect(page.getByText('Zoom-in speed')).toBeVisible();
});
