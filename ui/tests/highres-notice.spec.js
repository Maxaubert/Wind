// High resolution cursor notice (#441): turning the toggle ON asks first; "Don't show this again" is the
// global UI-only key uiHighResNoticeOff. Mock host: window.__live is the live ini, window.__msgs every post.
import { test, expect } from '@playwright/test';

test.beforeEach(async ({ page }) => {
  await page.addInitScript(() => {
    window.__skipSplash = true;
    window.__msgs = [];
    window.__live = { maxLevel: '12', model: 'hybrid', uiTheme: 'dark' };
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
const sw = (page) => page.locator('[data-key="txSamplingMode"]').getByRole('switch');
const notice = (page) => page.getByRole('dialog', { name: 'High resolution cursor is experimental' });
const writes = (page) => page.evaluate(() => window.__msgs.filter((m) => m.type === 'setConfig' || m.type === 'setConfigPersist')
  .map((m) => m.key + '=' + m.value));
const open = async (page) => { await page.goto('/'); await go(page, 'view'); };

test('turning it on shows the notice with its text, a labelled checkbox and both buttons', async ({ page }) => {
  await open(page);
  await sw(page).click({ force: true });
  const d = notice(page);
  await expect(d).toBeVisible();
  await expect(d).toContainText('It makes the zoomed image and the pointer smoother.');
  await expect(d).toContainText('which stops a little early.');
  await expect(d.getByRole('checkbox', { name: "Don't show this again" })).not.toBeChecked();
  await expect(d.getByRole('button', { name: 'Cancel' })).toBeVisible();
  await expect(d.getByRole('button', { name: 'Turn on' })).toBeFocused();   // focus moved into the dialog
  expect(await writes(page)).toEqual([]);   // nothing is written until Turn on
  expect(await d.evaluate((el) => !/\u2014/.test(el.textContent))).toBe(true);
});

test('Cancel and Esc leave it off and write nothing, even when the box is checked', async ({ page }) => {
  await open(page);
  await sw(page).click({ force: true });
  await notice(page).getByRole('checkbox').check();
  await notice(page).getByRole('button', { name: 'Cancel' }).click();
  await expect(notice(page)).toHaveCount(0);
  await expect(sw(page)).not.toBeChecked();
  await sw(page).click({ force: true });
  await expect(notice(page)).toBeVisible();
  await expect(notice(page).getByRole('checkbox')).not.toBeChecked();   // the box does not remember a cancelled try
  await page.keyboard.press('Escape');
  await expect(notice(page)).toHaveCount(0);
  await expect(sw(page)).not.toBeChecked();
  expect(await writes(page)).toEqual([]);
});

test('Turn on writes txSamplingMode=1 and nothing else', async ({ page }) => {
  await open(page);
  await sw(page).click({ force: true });
  await notice(page).getByRole('button', { name: 'Turn on' }).click();
  await expect(notice(page)).toHaveCount(0);
  await expect(sw(page)).toBeChecked();
  expect(await writes(page)).toEqual(['txSamplingMode=1']);
});

test('Tab stays inside the notice', async ({ page }) => {
  await open(page);
  await sw(page).click({ force: true });
  const inside = () => notice(page).evaluate((el) => el.contains(document.activeElement));
  for (let i = 0; i < 5; i++) { await page.keyboard.press('Tab'); expect(await inside()).toBe(true); }
  await page.keyboard.press('Shift+Tab');
  expect(await inside()).toBe(true);
});

test('checked + Turn on writes the opt-out key; a later off and on shows no notice', async ({ page }) => {
  await open(page);
  await sw(page).click({ force: true });
  await notice(page).getByRole('checkbox').check();
  await notice(page).getByRole('button', { name: 'Turn on' }).click();
  expect(await writes(page)).toEqual(['uiHighResNoticeOff=1', 'txSamplingMode=1']);
  await sw(page).click({ force: true });
  await expect(notice(page)).toHaveCount(0);
  await sw(page).click({ force: true });
  await expect(notice(page)).toHaveCount(0);
  await expect(sw(page)).toBeChecked();
  expect(await writes(page)).toEqual(['uiHighResNoticeOff=1', 'txSamplingMode=1', 'txSamplingMode=0', 'txSamplingMode=1']);
  await sw(page).click({ force: true });
  await expect(page.locator('.capsule')).toHaveCount(0);   // the opt-out key alone is never an unsaved change
});

test('unchecked Turn on keeps asking next time; turning off never asks', async ({ page }) => {
  await open(page);
  await sw(page).click({ force: true });
  await notice(page).getByRole('button', { name: 'Turn on' }).click();
  await sw(page).click({ force: true });
  await expect(notice(page)).toHaveCount(0);
  await sw(page).click({ force: true });
  await expect(notice(page)).toBeVisible();
  expect(await writes(page)).toEqual(['txSamplingMode=1', 'txSamplingMode=0']);
});

test('an ini that already has the opt-out shows no notice', async ({ page }) => {
  await page.addInitScript(() => { window.__live.uiHighResNoticeOff = '1'; window.__saved.uiHighResNoticeOff = '1'; });
  await open(page);
  await sw(page).click({ force: true });
  await expect(notice(page)).toHaveCount(0);
  expect(await writes(page)).toEqual(['txSamplingMode=1']);
});

test('a search result for the same row asks too, and Turn on writes through', async ({ page }) => {
  await page.goto('/');
  await page.locator('.side .search input').fill('high resolution cursor');
  const row = page.locator('.res [data-key="txSamplingMode"]');
  await row.getByRole('switch').click({ force: true });
  await expect(notice(page)).toBeVisible();
  await notice(page).getByRole('button', { name: 'Cancel' }).click();
  await expect(row.getByRole('switch')).not.toBeChecked();
  await row.getByRole('switch').click({ force: true });
  await notice(page).getByRole('button', { name: 'Turn on' }).click();
  await expect(row.getByRole('switch')).toBeChecked();
  expect(await writes(page)).toEqual(['txSamplingMode=1']);
});

for (const [w, h] of [[420, 700], [560, 800], [820, 560], [900, 360]]) {
  test(`the notice fits at ${w}x${h}`, async ({ page }) => {
    await page.setViewportSize({ width: w, height: h });
    await open(page);
    await sw(page).click({ force: true });
    const box = await notice(page).boundingBox();
    expect(box.x).toBeGreaterThanOrEqual(0);
    expect(box.y).toBeGreaterThanOrEqual(0);
    expect(box.x + box.width).toBeLessThanOrEqual(w);
    expect(box.y + box.height).toBeLessThanOrEqual(h);
    const btn = await notice(page).getByRole('button', { name: 'Turn on' }).boundingBox();
    expect(btn.y + btn.height).toBeLessThanOrEqual(h);
    expect(await notice(page).evaluate((el) => el.scrollWidth <= el.clientWidth)).toBe(true);
  });
}
