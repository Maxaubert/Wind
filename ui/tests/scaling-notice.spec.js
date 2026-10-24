// Smooth transform scaling notice (#441, formerly the High resolution cursor toggle): choosing Smooth asks first; "Don't show this again" is the
// global UI-only key uiHighResNoticeOff. Mock host: window.__live is the live ini, window.__msgs every post.
import { test, expect } from '@playwright/test';

test.beforeEach(async ({ page }) => {
  await page.addInitScript(() => {
    window.__skipSplash = true;
    window.__msgs = [];
    window.__live = { maxLevel: '12', model: 'transform', uiTheme: 'dark' };
    window.__saved = { ...window.__live };
    const listeners = new Set();
    const send = (data) => listeners.forEach((fn) => fn({ data }));
    window.chrome = { webview: {
      addEventListener: (_e, fn) => listeners.add(fn),
      postMessage: (msg) => {
        window.__msgs.push(msg);
        if (msg.type === 'getConfig') send({ type: 'config', values: { ...window.__live }, saved: { ...window.__saved },
          profiles: { names: ['Default'], active: 'Default' } });
        else if (msg.type === 'setConfig') window.__live[msg.key] = msg.value;
      },
    } };
  });
});

const go = (page, g) => page.locator('.side .it[data-g="' + g + '"]').click();
const sel = (page) => page.locator('[data-key="txSamplingMode"] select');
const on = (page, row = sel(page)) => row.selectOption('1');
const off = (page, row = sel(page)) => row.selectOption('0');
const notice = (page) => page.getByRole('dialog', { name: 'Smooth scaling is experimental' });
const writes = (page) => page.evaluate(() => window.__msgs.filter((m) => m.type === 'setConfig' || m.type === 'setConfigPersist')
  .map((m) => m.key + '=' + m.value));
const open = async (page) => { await page.goto('/'); await go(page, 'zoom'); };

test('choosing Smooth shows the notice with its text, a labelled checkbox and both buttons', async ({ page }) => {
  await open(page);
  await on(page);
  const d = notice(page);
  await expect(d).toBeVisible();
  await expect(d).toContainText('It makes the zoomed image and the pointer smoother.');
  await expect(d).toContainText('which stops a little early.');
  await expect(d.getByRole('checkbox', { name: "Don't show this again" })).not.toBeChecked();
  await expect(d.getByRole('button', { name: 'Cancel' })).toBeVisible();
  await expect(d.getByRole('button', { name: 'Use Smooth' })).toBeFocused();   // focus moved into the dialog
  expect(await writes(page)).toEqual([]);   // nothing is written until Turn on
  expect(await d.evaluate((el) => !/\u2014/.test(el.textContent))).toBe(true);
});

test('Cancel and Esc leave it on Nearest and write nothing, even when the box is checked', async ({ page }) => {
  await open(page);
  await on(page);
  await notice(page).getByRole('checkbox').check();
  await notice(page).getByRole('button', { name: 'Cancel' }).click();
  await expect(notice(page)).toHaveCount(0);
  await expect(sel(page)).toHaveValue('0');
  await on(page);
  await expect(notice(page)).toBeVisible();
  await expect(notice(page).getByRole('checkbox')).not.toBeChecked();   // the box does not remember a cancelled try
  await page.keyboard.press('Escape');
  await expect(notice(page)).toHaveCount(0);
  await expect(sel(page)).toHaveValue('0');
  expect(await writes(page)).toEqual([]);
});

test('Use Smooth writes txSamplingMode=1 and nothing else', async ({ page }) => {
  await open(page);
  await on(page);
  await notice(page).getByRole('button', { name: 'Use Smooth' }).click();
  await expect(notice(page)).toHaveCount(0);
  await expect(sel(page)).toHaveValue('1');
  expect(await writes(page)).toEqual(['txSamplingMode=1']);
});

test('Tab stays inside the notice', async ({ page }) => {
  await open(page);
  await on(page);
  const inside = () => notice(page).evaluate((el) => el.contains(document.activeElement));
  for (let i = 0; i < 5; i++) { await page.keyboard.press('Tab'); expect(await inside()).toBe(true); }
  await page.keyboard.press('Shift+Tab');
  expect(await inside()).toBe(true);
});

test('checked + Use Smooth writes the opt-out key; a later Nearest and Smooth shows no notice', async ({ page }) => {
  await open(page);
  await on(page);
  await notice(page).getByRole('checkbox').check();
  await notice(page).getByRole('button', { name: 'Use Smooth' }).click();
  expect(await writes(page)).toEqual(['uiHighResNoticeOff=1', 'txSamplingMode=1']);
  await off(page);
  await expect(notice(page)).toHaveCount(0);
  await on(page);
  await expect(notice(page)).toHaveCount(0);
  await expect(sel(page)).toHaveValue('1');
  expect(await writes(page)).toEqual(['uiHighResNoticeOff=1', 'txSamplingMode=1', 'txSamplingMode=0', 'txSamplingMode=1']);
  await off(page);
  await expect(page.locator('.capsule')).toHaveCount(0);   // the opt-out key alone is never an unsaved change
});

test('unchecked Use Smooth keeps asking next time; Nearest never asks', async ({ page }) => {
  await open(page);
  await on(page);
  await notice(page).getByRole('button', { name: 'Use Smooth' }).click();
  await off(page);
  await expect(notice(page)).toHaveCount(0);
  await on(page);
  await expect(notice(page)).toBeVisible();
  expect(await writes(page)).toEqual(['txSamplingMode=1', 'txSamplingMode=0']);
});

test('an ini that already has the opt-out shows no notice', async ({ page }) => {
  await page.addInitScript(() => { window.__live.uiHighResNoticeOff = '1'; window.__saved.uiHighResNoticeOff = '1'; });
  await open(page);
  await on(page);
  await expect(notice(page)).toHaveCount(0);
  expect(await writes(page)).toEqual(['txSamplingMode=1']);
});

test('a search result for the same row asks too, and Use Smooth writes through', async ({ page }) => {
  await page.goto('/');
  await page.locator('.side .search input').fill('high resolution cursor');
  const row = page.locator('.res [data-key="txSamplingMode"] select');
  await on(page, row);
  await expect(notice(page)).toBeVisible();
  await notice(page).getByRole('button', { name: 'Cancel' }).click();
  await expect(row).toHaveValue('0');
  await on(page, row);
  await notice(page).getByRole('button', { name: 'Use Smooth' }).click();
  await expect(row).toHaveValue('1');
  expect(await writes(page)).toEqual(['txSamplingMode=1']);
});

for (const [w, h] of [[420, 700], [560, 800], [820, 560], [900, 360]]) {
  test(`the notice fits at ${w}x${h}`, async ({ page }) => {
    await page.setViewportSize({ width: w, height: h });
    await open(page);
    await on(page);
    const box = await notice(page).boundingBox();
    expect(box.x).toBeGreaterThanOrEqual(0);
    expect(box.y).toBeGreaterThanOrEqual(0);
    expect(box.x + box.width).toBeLessThanOrEqual(w);
    expect(box.y + box.height).toBeLessThanOrEqual(h);
    const btn = await notice(page).getByRole('button', { name: 'Use Smooth' }).boundingBox();
    expect(btn.y + btn.height).toBeLessThanOrEqual(h);
    expect(await notice(page).evaluate((el) => el.scrollWidth <= el.clientWidth)).toBe(true);
  });
}
