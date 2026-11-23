// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
// The menu bar panel (panel.html, src/panel/Panel.svelte) against the mock host: the layout and its
// order from the menu* keys, slider, toggle and engine writes, the Mac banners, the palettes, the
// Performance header, the actions and the size report. The view model's pure parts are
// panel-model.nodetest.mjs.
import { test, expect } from '@playwright/test';
import { mockHost, sent } from './mock.js';

const openPanel = async (page, opts = {}) => {
  await mockHost(page, { init: true, ...opts });
  await page.goto('/panel.html');
  await expect(page.locator('.panel .card')).toBeVisible();
};
const ids = (loc) => loc.evaluateAll((els) => els.map((e) => e.dataset.id));
const actions = async (page) => (await sent(page, 'panelAction')).map((m) => m.action + (m.value ? ':' + m.value : ''));
const bg = (loc) => loc.evaluate((el) => getComputedStyle(el).backgroundColor);

test('defaults: header, Warmth and Brightness, three toggles, the engine field, Settings and Quit', async ({ page }) => {
  await openPanel(page);
  const card = page.locator('.card');
  expect((await card.boundingBox()).width).toBe(300);
  // The window is clear around the card (its shadow only): the page, the body and the panel root paint nothing.
  for (const sel of ['html', 'body', '.panel']) expect(await bg(page.locator(sel))).toBe('rgba(0, 0, 0, 0)');
  await expect(page.locator('.banner')).toHaveCount(0);
  await expect(page.locator('.head [data-zoom]')).toHaveText('Idle');
  await expect(page.locator('.head [data-ms]')).toHaveText('– ms');
  await expect(page.locator('.head .dots')).toHaveCount(1);
  expect(await ids(page.locator('.srow'))).toEqual(['slider:warmthPct', 'slider:dimPct']);
  expect(await ids(page.locator('.seg button'))).toEqual(['toggle:trackCaret', 'toggle:trackFocus', 'toggle:keepEdges']);
  await expect(page.locator('.engine .ev')).toHaveText('Transform');
  expect(await ids(page.locator('.bar .ib'))).toEqual(['settings', 'quit']);
  // Reading order: header, sliders, toggles, engine, bottom bar (Windows' flyout order).
  const ys = await page.locator('.head, .srow, .seg, .engine, .bar').evaluateAll((els) => els.map((e) => e.getBoundingClientRect().top));
  expect([...ys].sort((a, b) => a - b)).toEqual(ys);
  // Geometry from flyout_model.h: 96 px header, 40 px slider rows, 32 px controls, 40 px bar.
  expect((await page.locator('.head').boundingBox()).height).toBe(96);
  expect((await page.locator('.srow').first().boundingBox()).height).toBe(40);
  expect((await page.locator('.seg').boundingBox()).height).toBe(32);
  expect((await page.locator('.engine').boundingBox()).height).toBe(32);
  // The accessible names come from the items.
  await expect(page.getByRole('slider', { name: 'Warmth' })).toBeVisible();
  await expect(page.getByRole('button', { name: 'Follow the text cursor' })).toHaveAttribute('aria-pressed', 'true');
  await expect(page.getByRole('button', { name: 'Follow keyboard focus' })).toHaveAttribute('aria-pressed', 'false');
  await expect(page.getByRole('button', { name: 'Keep cursor centred' })).toHaveAttribute('aria-pressed', 'true');
});

test('the menu* keys pick and order what shows', async ({ page }) => {
  await openPanel(page, { values: {
    menuPerf: '0', menuSliders: 'satPct,maxLevel,panSpeed,panSmoothMs,warmthPct', menuSliderOrder: 'panSmoothMs,panSpeed,maxLevel',
    menuToggles: 'keepEdges,trackCaret', menuToggleOrder: 'keepEdges,engine,trackCaret,trackFocus' } });
  await expect(page.locator('.head')).toHaveCount(0);
  // Order key first, then the enabled list; the fifth enabled slider is past the cap of four.
  expect(await ids(page.locator('.srow'))).toEqual(['slider:panSmoothMs', 'slider:panSpeed', 'slider:maxLevel', 'slider:satPct']);
  expect(await ids(page.locator('.seg button'))).toEqual(['toggle:keepEdges', 'toggle:trackCaret']);
  await expect(page.locator('.engine')).toHaveCount(0);
});

test('nothing chosen: only the bottom bar', async ({ page }) => {
  await openPanel(page, { values: { menuPerf: '0', menuSliders: '', menuToggles: '' } });
  await expect(page.locator('.head, .qs')).toHaveCount(0);
  await expect(page.locator('.bar .ib')).toHaveCount(2);
});

test('sliders show the value as Settings does and write it', async ({ page }) => {
  await openPanel(page, { values: { menuSliders: 'warmthPct,maxLevel,zoomInSpeed,zoomEaseOutMs', warmthPct: '40', maxLevel: '12', zoomInSpeed: '1.75', zoomEaseOutMs: '115' } });
  expect(await page.locator('.srow .val').allTextContents()).toEqual(['40%', '12x', '1.75x', '115 ms']);
  const warm = page.getByRole('slider', { name: 'Warmth' });
  await warm.focus();
  await page.keyboard.press('ArrowRight');
  await expect.poll(async () => (await sent(page, 'setConfig')).at(-1)).toEqual({ type: 'setConfig', key: 'warmthPct', value: '45' });
  await expect(page.locator('[data-key="warmthPct"] .val')).toHaveText('45%');
  // A drag: many inputs, at most one write per 50 ms, the last value always lands (release flushes).
  await page.evaluate(() => {
    const el = document.querySelector('[data-key="maxLevel"] input');
    const set = (v) => { Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value').set.call(el, v); el.dispatchEvent(new Event('input', { bubbles: true })); };
    for (let v = 13; v <= 30; v++) set(String(v));
    el.dispatchEvent(new Event('change', { bubbles: true }));
  });
  const ml = (await sent(page, 'setConfig')).filter((m) => m.key === 'maxLevel');
  expect(ml.length).toBeLessThan(5);
  expect(ml.at(-1).value).toBe('30');
});

test('toggles write their keys; Keep cursor centred writes both alignments', async ({ page }) => {
  await openPanel(page);
  await page.getByRole('button', { name: 'Follow keyboard focus' }).click();
  await expect(page.getByRole('button', { name: 'Follow keyboard focus' })).toHaveAttribute('aria-pressed', 'true');
  await page.getByRole('button', { name: 'Keep cursor centred' }).click();
  await expect(page.getByRole('button', { name: 'Keep cursor centred' })).toHaveAttribute('aria-pressed', 'false');
  await page.getByRole('button', { name: 'Keep cursor centred' }).click();
  const w = (await sent(page, 'setConfigs')).map((m) => m.values);
  expect(w).toEqual([{ trackFocus: '1' }, { mouseAlign: '1', trackAlign: '1' }, { mouseAlign: '0', trackAlign: '0' }]);
  // A mixed state (only one alignment centred) reads off.
  await page.evaluate(() => window.__hostSend({ type: 'config', values: { ...window.__live, mouseAlign: '0', trackAlign: '1' } }));
  await expect(page.getByRole('button', { name: 'Keep cursor centred' })).toHaveAttribute('aria-pressed', 'false');
});

test('a refused write puts back the value Wind runs on', async ({ page }) => {
  await openPanel(page, { values: { warmthPct: '40' }, refuse: ['warmthPct'] });
  const warm = page.getByRole('slider', { name: 'Warmth' });
  await warm.focus();
  await page.keyboard.press('ArrowRight');
  await expect.poll(async () => (await sent(page, 'getConfig')).length).toBeGreaterThan(0);
  await expect(page.locator('[data-key="warmthPct"] .val')).toHaveText('40%');
});

test('a config pushed from elsewhere redraws the panel', async ({ page }) => {
  await openPanel(page);
  await page.evaluate(() => window.__hostSend({ type: 'config', push: true, values: { ...window.__live, warmthPct: '70', trackCaret: '0', engine: 'backdrop' } }));
  await expect(page.locator('[data-key="warmthPct"] .val')).toHaveText('70%');
  await expect(page.getByRole('button', { name: 'Follow the text cursor' })).toHaveAttribute('aria-pressed', 'false');
  await expect(page.locator('.engine .ev')).toHaveText('Backdrop');
});

test('the engine field opens Wind\'s list below it and picks', async ({ page }) => {
  await openPanel(page);
  const field = page.locator('.engine');
  await field.click();
  const list = page.getByRole('listbox', { name: 'Engine' });
  await expect(list).toBeVisible();
  await expect(list.getByRole('option')).toHaveText(['Transform', 'Render', 'Backdrop']);
  await expect(list.getByRole('option', { name: 'Transform' })).toHaveAttribute('aria-selected', 'true');
  const fb = await field.boundingBox(), lb = await list.boundingBox();
  expect(lb.y).toBeGreaterThanOrEqual(fb.y + fb.height);
  expect(Math.round(lb.width)).toBe(Math.round(fb.width));
  // The page reports a taller window while the list hangs below the card.
  const h = (await sent(page, 'panelSize')).at(-1).h;
  expect(h).toBeGreaterThanOrEqual(Math.ceil(lb.y + lb.height));
  await list.getByRole('option', { name: 'Render' }).click();
  await expect(list).toHaveCount(0);
  await expect(field.locator('.ev')).toHaveText('Render');
  expect(await actions(page)).toEqual(['setEngine:draw']);
  // Keyboard: open, down, Enter; Esc closes the list before the panel.
  await field.focus();
  await page.keyboard.press('Enter');
  await page.keyboard.press('ArrowDown');
  await page.keyboard.press('Enter');
  await expect(field.locator('.ev')).toHaveText('Backdrop');
  await page.keyboard.press('Enter');
  await page.keyboard.press('Escape');
  await expect(list).toHaveCount(0);
  expect(await actions(page)).toEqual(['setEngine:draw', 'setEngine:backdrop']);
});

test('Mac banners only while needed, each with its fix', async ({ page }) => {
  await openPanel(page, { mac: { accessibility: false, zoomShortcuts: ['Use keyboard shortcuts to zoom'] } });
  await expect(page.locator('.banner')).toHaveCount(2);
  await expect(page.locator('.banner[data-banner="accessibility"]')).toContainText('Wind needs Accessibility');
  await page.getByRole('button', { name: 'Open Privacy' }).click();
  await page.getByRole('button', { name: 'Turn off' }).click();
  expect(await actions(page)).toEqual(['openPrivacy', 'fixZoomShortcuts']);
  // Fixed: the host pushes the new status and the banners go.
  await page.evaluate(() => window.__hostSend({ type: 'macStatus', accessibility: true, screenRecording: true, zoomShortcuts: [] }));
  await expect(page.locator('.banner')).toHaveCount(0);
});

test('the Performance header shows the level, fps, the trace and the mean', async ({ page }) => {
  await openPanel(page);
  await page.evaluate(() => window.__hostSend({ type: 'perf', level: 7.42, ms: [...Array(120).fill(6.9), 20, ...Array(40).fill(6.9)] }));
  await expect(page.locator('[data-zoom]')).toHaveText('7.4x');
  await expect(page.locator('[data-zoom]')).not.toHaveClass(/idle/);
  await expect(page.locator('[data-fps]')).toHaveText('143 fps');
  await expect(page.locator('[data-ms]')).toHaveText('7.0 ms');
  const pts = (await page.locator('.spark polyline').getAttribute('points')).split(' ');
  expect(pts.length).toBe(64);
  await page.evaluate(() => window.__hostSend({ type: 'perf', level: 1, ms: [] }));
  await expect(page.locator('[data-zoom]')).toHaveText('Idle');
  await expect(page.locator('[data-fps]')).toHaveCount(0);
});

test('Settings, Quit and Esc ask the host; closing drops focus so a slider flushes', async ({ page }) => {
  await openPanel(page);
  await page.getByRole('button', { name: 'Settings' }).click();
  await page.getByRole('button', { name: 'Quit Wind' }).click();
  await page.keyboard.press('Escape');
  expect(await actions(page)).toEqual(['openSettings', 'quit', 'close']);
  await page.getByRole('slider', { name: 'Brightness' }).focus();
  await page.evaluate(() => window.__hostSend({ type: 'panel', open: false }));
  expect(await page.evaluate(() => document.activeElement === document.body)).toBe(true);
});

test('Left and Right walk the toggle group', async ({ page }) => {
  await openPanel(page);
  await page.getByRole('button', { name: 'Follow the text cursor' }).focus();
  await page.keyboard.press('ArrowRight');
  await expect(page.getByRole('button', { name: 'Follow keyboard focus' })).toBeFocused();
  await page.keyboard.press('ArrowRight');
  await page.keyboard.press('ArrowRight');
  await expect(page.getByRole('button', { name: 'Keep cursor centred' })).toBeFocused();
  await page.keyboard.press('ArrowLeft');
  await expect(page.getByRole('button', { name: 'Follow keyboard focus' })).toBeFocused();
});

test('hover changes a segment and is reported to the host', async ({ page }) => {
  await openPanel(page);
  const seg = page.getByRole('button', { name: 'Follow keyboard focus' });
  const hl = seg.evaluate((el) => getComputedStyle(el, '::before').opacity);
  expect(await hl).toBe('0');
  await seg.hover();
  await expect.poll(() => seg.evaluate((el) => getComputedStyle(el, '::before').opacity)).toBe('1');
  await expect.poll(async () => (await sent(page, 'hover')).at(-1)?.id).toBe('toggle:trackFocus');
});

test('the page reports its height: the card plus the bottom margin', async ({ page }) => {
  await openPanel(page);
  await expect.poll(async () => (await sent(page, 'panelSize')).length).toBeGreaterThan(0);
  const card = await page.locator('.card').boundingBox();
  expect((await sent(page, 'panelSize')).at(-1).h).toBe(Math.ceil(card.y + card.height + 14));
});

for (const [pal, on, radius] of [['grey', 'rgb(47, 191, 165)', '8px'], ['ember', 'rgb(228, 138, 69)', '8px'],
  ['ocean', 'rgb(90, 170, 245)', '8px'], ['hicon', 'rgb(255, 255, 255)', '3px']]) {
  test(`palette ${pal} (uiPalette) colours the panel`, async ({ page }) => {
    await openPanel(page, { values: { uiPalette: pal } });
    await expect(page.locator('.panel')).toHaveAttribute('data-palette', pal);
    expect(await bg(page.getByRole('button', { name: 'Follow the text cursor' }))).toBe(on);
    expect(await page.locator('.seg').evaluate((el) => getComputedStyle(el).borderTopLeftRadius)).toBe(radius);
  });
}

test('an unknown palette falls back to Wind grey', async ({ page }) => {
  await openPanel(page, { values: { uiPalette: 'nope' } });
  await expect(page.locator('.panel')).toHaveAttribute('data-palette', 'grey');
});

test('reduced motion: no transitions', async ({ page }) => {
  await page.emulateMedia({ reducedMotion: 'reduce' });
  await openPanel(page);
  const d = await page.locator('.seg button').first().evaluate((el) => getComputedStyle(el).transitionDuration);
  expect(d.split(',').every((x) => parseFloat(x) < 0.001)).toBe(true);
});
