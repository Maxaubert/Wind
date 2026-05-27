import { test, expect } from '@playwright/test';

test.beforeEach(async ({ page }) => {
  await page.addInitScript(() => {
    window.__sets = [];
    const listeners = new Set();
    window.chrome = { webview: {
      addEventListener: (_e, fn) => listeners.add(fn),
      postMessage: (msg) => {
        if (msg.type === 'getConfig')
          listeners.forEach(fn => fn({ data: { type: 'config', values: { zoomInSpeed: '1.2', smoothZoom: '0', uiTheme: 'auto', zoomInButton: '2', zoomInVk: '33', zoomOutButton: '1', zoomOutVk: '34' } } }));
        if (msg.type === 'setConfig') window.__sets.push(msg);
      },
    }};
  });
});

test('renders all sections on one page', async ({ page }) => {
  await page.goto('/');
  await expect(page.getByText('Zoom-in speed')).toBeVisible();
  await expect(page.getByText('Cursor speed')).toBeVisible();
  await expect(page.getByText('Sharpness')).toBeVisible();
  await expect(page.getByRole('heading', { name: 'About' })).toBeVisible();
});

test('rail click scrolls and marks the section active', async ({ page }) => {
  await page.goto('/');
  await page.getByRole('button', { name: 'Display' }).click();
  await expect(page.getByRole('button', { name: 'Display' })).toHaveClass(/active/);
});

test('theme toggle writes uiTheme', async ({ page }) => {
  await page.goto('/');
  await page.getByRole('button', { name: 'Toggle theme' }).click();
  const sets = await page.evaluate(() => window.__sets);
  expect(sets.some(s => s.key === 'uiTheme')).toBeTruthy();
});

test('changes stage until Apply, then setConfig fires', async ({ page }) => {
  await page.goto('/');
  await page.getByText('Smooth zoom', { exact: true }).locator('xpath=../..').getByRole('checkbox').click();
  expect(await page.evaluate(() => window.__sets.filter(s => s.key === 'smoothZoom').length)).toBe(0);
  await page.getByRole('button', { name: 'Apply' }).click();
  const sets = await page.evaluate(() => window.__sets);
  expect(sets.some(s => s.key === 'smoothZoom' && s.value === '1')).toBeTruthy();
});

test('keybind capture writes a VK on keydown', async ({ page }) => {
  await page.goto('/');
  await page.getByText('Zoom in', { exact: true }).locator('xpath=../..').getByRole('button').click();
  await page.keyboard.press('F2'); // keyCode 113
  await page.getByRole('button', { name: 'Apply' }).click();
  const sets = await page.evaluate(() => window.__sets);
  expect(sets.some(s => s.key === 'zoomInVk' && s.value === '113')).toBeTruthy();
});
