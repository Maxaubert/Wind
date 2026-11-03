// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
// The Settings window against the mock Mac host: five pages, live apply (one setConfig per change, no
// Save), the advanced switch, showIf/offIf, the engine switch and its macOS zoom rows, pushed configs,
// a refused write, and the Mac shortcuts.
import { test, expect } from '@playwright/test';
import { mockHost, open, go, row, sent, wrote, live, advanced, CMD } from './mock.js';

test.beforeEach(async ({ page }) => { await mockHost(page, { values: { maxLevel: '12' } }); });

// ---- structure ------------------------------------------------------------------------------------

test('Settings opens on Hotkeys; three pages, a divider, then Preferences and About', async ({ page }) => {
  await open(page);
  await expect(page.locator('h1')).toHaveText('Hotkeys');
  await expect(page.locator('.side nav:not(.bottom) .it')).toHaveText([/Hotkeys/, /Zoom/, /View/]);
  await expect(page.locator('.side nav.bottom .it')).toHaveText([/Preferences/, /About/]);
  for (const [g, title] of [['zoom', 'Zoom'], ['view', 'View'], ['prefs', 'Preferences'], ['hotkeys', 'Hotkeys']]) {
    await go(page, g);
    await expect(page.locator('h1')).toHaveText(title);
    await expect(page.locator('.side .it.sel')).toHaveAttribute('data-g', g);
    await expect(page.locator('.side .it.sel')).toHaveAttribute('aria-current', 'page');
  }
  await go(page, 'about');
  await expect(page.locator('main .about .name')).toHaveText('Wind');
  await expect(page.getByRole('button', { name: /GitHub/ })).toHaveCount(0);
});

test('pages show their cards; the macOS zoom card is hidden on the render engine', async ({ page }) => {
  await mockHost(page, { values: { maxLevel: '12', engine: 'draw' } });
  await open(page);
  await expect(page.locator('main .cap')).toHaveText(['Zoom', 'Quick zoom', 'Extra keys']);
  await go(page, 'zoom');
  await expect(page.locator('main .cap')).toHaveText(['Level and speed', 'Engine']);   // Easing is advanced
  await go(page, 'view');
  await expect(page.locator('main .cap')).toHaveText(['Movement', 'Typing and focus']);
  await go(page, 'prefs');
  await expect(page.locator('main .cap')).toHaveText(['General', 'Screen light', 'macOS']);
});

test('values come from the host config and missing keys fall back to the defaults', async ({ page }) => {
  await open(page);
  await go(page, 'zoom');
  await expect(row(page, 'maxLevel').locator('input[type=range]')).toHaveValue('12');
  await expect(row(page, 'zoomInSpeed').locator('input[type=range]')).toHaveValue('1.75');
  await expect(row(page, 'zoomInSpeed').locator('.val')).toHaveText('1.75x');
});

// ---- live apply -----------------------------------------------------------------------------------

test('a slider, a toggle and a select each write one key at once', async ({ page }) => {
  await open(page);
  await go(page, 'zoom');
  const sl = row(page, 'maxLevel').locator('input[type=range]');
  await sl.focus();
  await page.keyboard.press('ArrowRight');
  await expect.poll(() => live(page, 'maxLevel')).toBe('13');
  await go(page, 'view');
  await row(page, 'trackFocus').getByRole('switch').check({ force: true });
  await expect.poll(() => live(page, 'trackFocus')).toBe('1');
  await row(page, 'mouseAlign').locator('select').selectOption('1');
  await expect.poll(() => live(page, 'mouseAlign')).toBe('1');
  const keys = (await sent(page, 'setConfig')).map((m) => m.key);
  expect(keys).toEqual(['maxLevel', 'trackFocus', 'mouseAlign']);
  // Nothing session-like is ever sent.
  for (const t of ['setConfigPersist', 'saveSession', 'discardSession', 'dirty']) expect(await sent(page, t), t).toEqual([]);
});

test('a slider drag is throttled but always ends on the released value', async ({ page }) => {
  await open(page);
  await go(page, 'zoom');
  const sl = row(page, 'zoomInSpeed').locator('input[type=range]');
  await sl.focus();
  for (let i = 0; i < 10; i++) await page.keyboard.press('ArrowRight');
  await expect.poll(() => live(page, 'zoomInSpeed')).toBe('2.25');
  expect((await sent(page, 'setConfig')).length).toBeLessThanOrEqual(10);
});

test('showIf: Scroll zoom speed only while the wheel has modifiers; Edge margin only when within the edges', async ({ page }) => {
  await open(page);
  await expect(row(page, 'wheelZoomSpeed')).toBeVisible();
  await page.evaluate(() => window.__hostSend({ type: 'config', push: true, values: { ...window.__live, wheelZoomMods: '' } }));
  await expect(row(page, 'wheelZoomSpeed')).toHaveCount(0);
  await go(page, 'view');
  await expect(row(page, 'mouseMarginPct')).toHaveCount(0);
  await row(page, 'mouseAlign').locator('select').selectOption('1');
  await expect(row(page, 'mouseMarginPct')).toBeVisible();
  await expect(row(page, 'trackMarginPct')).toHaveCount(0);
  await row(page, 'trackAlign').locator('select').selectOption('1');
  await expect(row(page, 'trackMarginPct')).toBeVisible();
});

test('offIf: the soft start sliders dim while Soft start is off', async ({ page }) => {
  await open(page);
  await advanced(page, true);
  await go(page, 'zoom');
  await expect(page.locator('main .cap')).toHaveText(['Level and speed', 'Easing', 'Engine']);
  await expect(row(page, 'smoothZoomAccel')).not.toHaveClass(/disabled/);
  await row(page, 'smoothZoom').getByRole('switch').uncheck({ force: true });
  await expect(row(page, 'smoothZoomAccel')).toHaveClass(/disabled/);
  await expect(row(page, 'smoothZoomRamp')).toHaveClass(/disabled/);
  await expect(row(page, 'smoothZoomRamp').locator('input')).toBeDisabled();
});

test('the advanced switch shows the advanced rows inline and hides them again', async ({ page }) => {
  await open(page);
  await go(page, 'zoom');
  await expect(row(page, 'smoothZoom')).toHaveCount(0);
  await advanced(page, true);
  await expect.poll(() => live(page, 'showAdvanced')).toBe('1');
  await go(page, 'zoom');
  await expect(row(page, 'smoothZoom')).toBeVisible();
  await advanced(page, false);
  await go(page, 'zoom');
  await expect(row(page, 'smoothZoom')).toHaveCount(0);
});

// ---- engine ---------------------------------------------------------------------------------------

test('Engine defaults to Transform (owner 2026-11-02), and its rows show on View', async ({ page }) => {
  await open(page);
  await go(page, 'zoom');
  await expect(row(page, 'engine').locator('select')).toHaveValue('zoom');
  await go(page, 'view');
  await expect(page.locator('main .cap')).toHaveText(['Movement', 'Typing and focus', 'Transform engine']);
});

test('Scaling: Render offers Nearest and xBR, Transform only macOS\'s two', async ({ page }) => {
  await mockHost(page, { values: { engine: 'draw' } });
  await open(page);
  await go(page, 'zoom');
  await expect(row(page, 'smoothImages')).toHaveCount(0);
  const sel = row(page, 'renderScaling').locator('select');
  await expect(sel).toHaveValue('0');
  await expect(sel.locator('option')).toHaveText(['Nearest', 'xBR']);
  await sel.selectOption({ label: 'xBR' });
  await expect.poll(() => wrote(page, 'renderScaling')).toBe('1');
  await sel.selectOption({ label: 'Nearest' });
  await expect.poll(() => wrote(page, 'renderScaling')).toBe('0');
  await row(page, 'engine').locator('select').selectOption('zoom');
  await expect(row(page, 'renderScaling')).toHaveCount(0);
  const tx = row(page, 'smoothImages').locator('select');
  await expect(tx.locator('option')).toHaveText(['Nearest', 'Smooth']);
  await expect(row(page, 'smoothImages')).toContainText('only these two');
});

test('Engine shows on Zoom without Advanced; on Render, Transform brings its rows', async ({ page }) => {
  await mockHost(page, { values: { maxLevel: '12', engine: 'draw' } });
  await open(page);
  await go(page, 'zoom');
  const sel = row(page, 'engine').locator('select');
  await expect(sel).toHaveValue('draw');
  await expect(sel.locator('option')).toHaveText(['Render', 'Transform']);
  await go(page, 'view');
  for (const k of ['slowPrecision', 'slowPanGlide', 'cursor', 'precisionBelow'])
    await expect(row(page, k), k).toHaveCount(0);
  await go(page, 'zoom');
  await sel.selectOption('zoom');
  await expect.poll(() => wrote(page, 'engine')).toBe('zoom');
  await expect(page.locator('[role=status]').filter({ hasText: 'Engine set to Transform' })).toHaveCount(1);
  await advanced(page, true);                     // precisionBelow is an advanced row
  await go(page, 'view');
  await expect(page.locator('main .cap')).toHaveText(['Movement', 'Typing and focus', 'Transform engine']);
  for (const k of ['slowPrecision', 'slowPanGlide', 'cursor', 'precisionBelow'])
    await expect(row(page, k), k).toBeVisible();
  await expect(row(page, 'slowPanGlide').locator('.tag')).toHaveText('Experimental');
  await expect(row(page, 'precisionBelow')).toHaveClass(/disabled/);   // slowPrecision is off
  await row(page, 'slowPrecision').getByRole('switch').check({ force: true });
  await expect(row(page, 'precisionBelow')).not.toHaveClass(/disabled/);
  await expect(row(page, 'cursor').locator('select')).toHaveValue('wind');   // Transform's default: Wind draws the pointer
  await row(page, 'cursor').locator('select').selectOption('system');
  await expect.poll(() => live(page, 'cursor')).toBe('system');
});

// ---- the host -------------------------------------------------------------------------------------

test('a pushed config (a menu or text editor edit) refreshes the page', async ({ page }) => {
  await open(page);
  await go(page, 'zoom');
  await page.evaluate(() => window.__hostSend({ type: 'config', push: true, values: { ...window.__live, maxLevel: '40' } }));
  await expect(row(page, 'maxLevel').locator('input[type=range]')).toHaveValue('40');
});

test('a push right after a write does not pull the control back', async ({ page }) => {
  await open(page);
  await go(page, 'zoom');
  const sl = row(page, 'maxLevel').locator('input[type=range]');
  await sl.focus();
  await page.keyboard.press('ArrowRight');   // 13
  await page.evaluate(() => window.__hostSend({ type: 'config', push: true, values: { ...window.__live, maxLevel: '12' } }));
  await expect(sl).toHaveValue('13');
});

test('a refused write shows a prompt and reads the config again', async ({ page }) => {
  await page.addInitScript(() => { window.__refuse.push('trackFocus'); });
  await open(page);
  await go(page, 'view');
  const sw = row(page, 'trackFocus').getByRole('switch');
  await sw.click({ force: true });   // the host refuses it, so the switch goes back
  const dlg = page.getByRole('dialog');
  await expect(dlg).toContainText("Couldn't save the setting");
  await expect(dlg).toContainText('trackFocus');
  await expect.poll(async () => (await sent(page, 'getConfig')).length).toBeGreaterThanOrEqual(1);
  await expect(sw).not.toBeChecked();
  await dlg.getByRole('button', { name: 'Close' }).click();
  await expect(page.getByRole('dialog')).toHaveCount(0);
});

test('the host-injected first paint is used without a round trip', async ({ page }) => {
  await page.addInitScript(() => { window.__windInit = { values: { maxLevel: '20', uiPalette: 'ocean' }, runningModel: 'draw', mac: { accessibility: true, screenRecording: true, zoomShortcuts: [], loginItem: true } }; });
  await open(page);
  expect(await sent(page, 'getConfig')).toEqual([]);
  await expect(page.locator('.app')).toHaveAttribute('data-palette', 'ocean');
  await go(page, 'prefs');
  await expect(row(page, '__loginItem').getByRole('switch')).toBeChecked();
});

// ---- Mac shortcuts --------------------------------------------------------------------------------

test('Cmd+F focuses search, Cmd+Q asks the host to quit Wind; Ctrl+F does nothing', async ({ page }) => {
  await open(page);
  await page.locator('main').click({ position: { x: 5, y: 5 } });
  await page.keyboard.press('Control+f');
  await expect(page.getByLabel('Search settings')).not.toBeFocused();
  await page.keyboard.press(CMD + '+f');
  await expect(page.getByLabel('Search settings')).toBeFocused();
  await page.locator('main').click({ position: { x: 5, y: 5 } });
  await page.keyboard.press(CMD + '+q');
  expect((await sent(page, 'window')).map((m) => m.action)).toEqual(['quitWind']);
});

// ---- the Hotkeys page -----------------------------------------------------------------------------

test('hotkeys rows show the Mac binds as boxes with glyphs', async ({ page }) => {
  await open(page);
  await expect(row(page, 'zoomIn').locator('.kchg .kc')).toHaveText(['Mouse 4', '⌃⌥=']);
  await expect(row(page, 'zoomOut').locator('.kchg .kc')).toHaveText(['Mouse 3', '⌃⌥-']);
  await expect(row(page, 'wheelZoomMods').locator('.kchg .kc')).toHaveText(['⌃+Scroll']);
  await expect(row(page, '__pan').locator('.kchg .kc')).toContainText('Arrow keys');
  await expect(row(page, 'hideCursor').getByRole('button', { name: 'Set Hide pointer key' })).toBeVisible();
  // No on/off switches on the Hotkeys page.
  await expect(page.locator('main').getByRole('switch')).toHaveCount(0);
});

// ---- select list and slider keys (owner 2026-11-01) ---------------------------------------------------

test('a select opens its own list below the button, not the native menu over it', async ({ page }) => {
  await mockHost(page, { values: { maxLevel: '12', engine: 'draw' } });
  await open(page);
  await go(page, 'zoom');
  const sel = row(page, 'engine').locator('select');
  await sel.click();
  const list = page.getByRole('listbox');
  await expect(list).toBeVisible();
  await expect(list.getByRole('option')).toHaveText(['Render', 'Transform']);
  await expect(list.getByRole('option', { name: 'Render' })).toHaveAttribute('aria-selected', 'true');
  const s = await sel.boundingBox(), l = await list.boundingBox();
  expect(l.y).toBeGreaterThanOrEqual(s.y + s.height);              // below the button, never over it
  await list.getByRole('option', { name: 'Transform' }).click();
  await expect(list).toHaveCount(0);
  await expect.poll(() => wrote(page, 'engine')).toBe('zoom');
  await expect(sel).toHaveValue('zoom');
  // Keyboard: Space opens, arrows move, Enter picks; Escape closes without a change.
  await sel.focus();
  await page.keyboard.press(' ');
  await expect(list).toBeVisible();
  await page.keyboard.press('ArrowUp');
  await page.keyboard.press('Enter');
  await expect.poll(() => wrote(page, 'engine')).toBe('draw');
  await expect(sel).toBeFocused();
  await page.keyboard.press('ArrowDown');
  await expect(list).toBeVisible();
  await page.keyboard.press('Escape');
  await expect(list).toHaveCount(0);
  await expect(sel).toHaveValue('draw');
  // A press outside closes it.
  await sel.click();
  await page.locator('h1').click();
  await expect(list).toHaveCount(0);
});

test('after dragging a slider the arrow keys step it by its increment, with no focus ring', async ({ page }) => {
  await open(page);
  await go(page, 'zoom');
  const sl = row(page, 'zoomInSpeed').locator('input[type=range]');
  const b = await sl.boundingBox();
  await page.mouse.move(b.x + b.width * 0.3, b.y + b.height / 2);
  await page.mouse.down();
  await page.mouse.move(b.x + b.width * 0.5, b.y + b.height / 2, { steps: 4 });
  await page.mouse.up();
  await expect(sl).toBeFocused();
  const v0 = Number(await sl.inputValue());
  await page.keyboard.press('ArrowRight');
  await expect.poll(async () => Number(await sl.inputValue())).toBeCloseTo(v0 + 0.05, 5);
  await page.keyboard.press('ArrowLeft');
  await page.keyboard.press('ArrowLeft');
  await expect.poll(async () => Number(await sl.inputValue())).toBeCloseTo(v0 - 0.05, 5);
  await expect.poll(async () => Number(await live(page, 'zoomInSpeed'))).toBeCloseTo(v0 - 0.05, 5);
  expect(await sl.evaluate((e) => getComputedStyle(e).outlineStyle)).toBe('none');
  // A second press on the focused slider keeps it quiet too.
  await page.mouse.click(b.x + b.width * 0.6, b.y + b.height / 2);
  await expect(sl).toBeFocused();
  await page.keyboard.press('ArrowRight');
  expect(await sl.evaluate((e) => getComputedStyle(e).outlineStyle)).toBe('none');
});
