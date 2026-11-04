// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
// Mac keybinds: the pure box model (lib/bindings.js), then the Hotkeys page recording real key presses
// and mouse buttons against the mock host. The rule table itself is tested in
// keybind-rules.nodetest.mjs against the fixture Swift also reads.
import { test, expect } from '@playwright/test';
import { unitsOf, placePatch, removePatch, maxOf, panState, panPatch, eventMods, keyEventBind, mouseEventBind } from '../src/lib/bindings.js';
import { mockHost, open, row, sent, wrote, live } from './mock.js';

const zoomIn = { key: 'zoomIn', type: 'keybind', max: 3 };
const pan = { key: '__pan', panKeys: ['panLeft', 'panUp', 'panRight', 'panDown'] };

// ---- the model --------------------------------------------------------------------------------

test('units: one box per bind, glyphs tight, unusable text kept so it can be removed', () => {
  const u = unitsOf(zoomIn, { zoomIn: 'mouse4,ctrl+opt+equal,a,ctrl+bogus' });
  expect(u.map((x) => x.label)).toEqual(['Mouse 4', '⌃⌥=', 'A', 'ctrl+bogus']);   // refused, unreadable
  expect(u.map((x) => x.ok)).toEqual([true, true, false, false]);
  expect(unitsOf(zoomIn, { zoomIn: '' })).toEqual([]);
  expect(maxOf(zoomIn)).toBe(3);
  expect(maxOf({ key: 'x' })).toBe(1);
});

test('place and remove patches keep the list order and never store a bind twice', () => {
  const v = { zoomIn: 'mouse4,ctrl+opt+equal' };
  expect(placePatch(zoomIn, v, null, 'f18')).toEqual({ zoomIn: 'mouse4,ctrl+opt+equal,f18' });
  expect(placePatch(zoomIn, v, 0, 'mouse5')).toEqual({ zoomIn: 'mouse5,ctrl+opt+equal' });
  expect(placePatch(zoomIn, v, 0, 'ctrl+opt+equal')).toEqual({ zoomIn: 'ctrl+opt+equal' });
  expect(removePatch(zoomIn, v, 0)).toEqual({ zoomIn: 'ctrl+opt+equal' });
  expect(removePatch(zoomIn, { zoomIn: 'f18' }, 0)).toEqual({ zoomIn: '' });
});

test('events: Option is opt, Command is cmd; DOM mouse buttons map to the Mac numbers', () => {
  expect(eventMods({ ctrlKey: true, altKey: true, shiftKey: false, metaKey: true })).toEqual(['ctrl', 'opt', 'cmd']);
  expect(keyEventBind({ code: 'KeyK', ctrlKey: true })).toEqual({ kind: 'key', bind: { mods: ['ctrl'], key: 'k' } });
  expect(keyEventBind({ code: 'AltLeft' })).toEqual({ kind: 'mod' });
  expect(keyEventBind({ code: 'Unidentified' })).toBeNull();
  expect(mouseEventBind({ button: 1 })).toEqual({ mods: [], mouse: 2 });
  expect(mouseEventBind({ button: 3 })).toEqual({ mods: [], mouse: 3 });
  expect(mouseEventBind({ button: 4 })).toEqual({ mods: [], mouse: 4 });
});

test('pan: none, arrows with one shared modifier set, custom for anything else', () => {
  expect(panState(pan, {}).kind).toBe('none');
  expect(panState(pan, { panLeft: 'ctrl+opt+left', panUp: 'ctrl+opt+up', panRight: 'ctrl+opt+right', panDown: 'ctrl+opt+down' }))
    .toEqual({ kind: 'arrows', mods: ['ctrl', 'opt'] });
  expect(panState(pan, { panLeft: 'ctrl+left', panUp: 'ctrl+opt+up', panRight: 'ctrl+opt+right', panDown: 'ctrl+opt+down' }).kind).toBe('custom');
  expect(panPatch(pan, ['cmd'])).toEqual({ panLeft: 'cmd+left', panUp: 'cmd+up', panRight: 'cmd+right', panDown: 'cmd+down' });
  expect(panPatch(pan, [])).toEqual({ panLeft: '', panUp: '', panRight: '', panDown: '' });
});

// ---- the page ---------------------------------------------------------------------------------

test.describe('the Hotkeys page', () => {
  test.beforeEach(async ({ page }) => { await mockHost(page); await open(page); });

  test('add a key: + records a combination and writes the whole list', async ({ page }) => {
    const r = row(page, 'zoomIn');
    await r.getByRole('button', { name: 'Add Zoom in key' }).click();
    await expect(r.locator('.kc.live')).toHaveText('Press keys...');
    await page.keyboard.press('Control+Alt+KeyP');
    await expect.poll(() => live(page, 'zoomIn')).toBe('mouse4,ctrl+opt+equal,ctrl+opt+p');
    await expect(r.locator('.kchg .kc')).toHaveText(['Mouse 4', '⌃⌥=', '⌃⌥P']);
    // Three is the limit: no + box any more.
    await expect(r.locator('.ghost')).toHaveCount(0);
  });

  test('click a box to record it again in place', async ({ page }) => {
    const r = row(page, 'zoomIn');
    await r.locator('.kchg').nth(1).click();
    await page.keyboard.press('F12');   // F keys need no modifier
    await expect.poll(() => live(page, 'zoomIn')).toBe('mouse4,f12');
  });

  test('the x and a right-click remove a bind', async ({ page }) => {
    const r = row(page, 'zoomIn');
    await r.locator('.kb').first().hover();
    await r.getByRole('button', { name: /^Remove binding Mouse 4/ }).click();
    await expect.poll(() => live(page, 'zoomIn')).toBe('ctrl+opt+equal');
    await r.locator('.kchg').first().click({ button: 'right' });
    await expect.poll(() => live(page, 'zoomIn')).toBe('');
    await expect(r.getByRole('button', { name: 'Set Zoom in key' })).toBeVisible();
  });

  test('a refused combination stays armed and says why; Escape cancels without writing', async ({ page }) => {
    const r = row(page, 'inspect');
    await r.locator('.kchg').first().click();
    await page.keyboard.press('KeyA');
    await expect(r.locator('.refusal')).toContainText('alone would stop you typing');
    await page.keyboard.press('Alt+KeyA');
    await expect(r.locator('.refusal')).toContainText('types a character on macOS');
    await page.keyboard.press('Meta+KeyA');
    await expect(r.locator('.refusal')).toContainText('used by app shortcuts');
    await page.keyboard.press('Escape');
    await expect(r.locator('.kc.live')).toHaveCount(0);
    expect(await wrote(page, 'inspect')).toBeUndefined();
  });

  test('a mouse button binds; the left button is refused', async ({ page }) => {
    const r = row(page, 'hideCursor');
    await r.getByRole('button', { name: 'Set Hide pointer key' }).click();
    await r.locator('.ghost').click({ button: 'middle' });
    await expect.poll(() => live(page, 'hideCursor')).toBe('mouse2');
    await expect(r.locator('.kchg .kc')).toHaveText(['Middle click']);
  });

  test('wheel row: hold modifiers and let go records modifiers only; x turns it off', async ({ page }) => {
    const r = row(page, 'wheelZoomMods');
    await r.locator('.kchg').click();
    await expect(r.locator('.kc.live')).toHaveText('Hold modifiers...');
    await page.keyboard.down('Control');
    await page.keyboard.down('Alt');
    await page.keyboard.up('Alt');
    await page.keyboard.up('Control');
    await expect.poll(() => live(page, 'wheelZoomMods')).toBe('ctrl+opt');
    await expect(r.locator('.kchg .kc')).toHaveText(['⌃⌥+Scroll']);
    await r.locator('.kb').hover();
    await r.locator('.kx').click();
    await expect.poll(() => live(page, 'wheelZoomMods')).toBe('');
    await expect(row(page, 'wheelZoomSpeed')).toHaveCount(0);   // Scroll zoom speed hides with it
  });

  test('pan: modifiers plus an arrow write all four lists in one go; remove clears all four', async ({ page }) => {
    const r = row(page, '__pan');
    await expect(r.locator('.kchg .kc')).toContainText('⌃⌥');
    await r.locator('.kchg').click();
    await page.keyboard.down('Meta');
    await page.keyboard.down('Alt');
    await page.keyboard.press('ArrowLeft');
    await page.keyboard.up('Alt');
    await page.keyboard.up('Meta');
    await expect.poll(() => live(page, 'panUp')).toBe('opt+cmd+up');
    expect(await page.evaluate(() => ['panLeft', 'panUp', 'panRight', 'panDown'].map((k) => window.__live[k])))
      .toEqual(['opt+cmd+left', 'opt+cmd+up', 'opt+cmd+right', 'opt+cmd+down']);
    await r.locator('.kb').hover();
    await r.getByRole('button', { name: 'Remove pan keys' }).click();
    await expect.poll(() => page.evaluate(() => ['panLeft', 'panUp', 'panRight', 'panDown'].map((k) => window.__live[k]).join('|'))).toBe('|||');
    await expect(r.getByRole('button', { name: 'Set pan modifiers' })).toBeVisible();
  });

  test('recording pauses Wind\'s hotkeys and resumes them after', async ({ page }) => {
    await row(page, 'inspect').locator('.kchg').first().click();
    await page.keyboard.press('Escape');
    const on = (await sent(page, 'captureBinds')).map((m) => m.on);
    expect(on).toContain('1');
    expect(on.at(-1)).toBe('0');
  });

  test('while recording, Cmd+F and Cmd+Q are captured, not run', async ({ page }) => {
    const r = row(page, 'inspect');
    await r.locator('.kchg').first().click();
    await page.keyboard.press('Meta+KeyQ');
    await expect(r.locator('.refusal')).toContainText('used by app shortcuts');
    expect(await sent(page, 'window')).toEqual([]);
    await expect(page.getByLabel('Search settings')).not.toBeFocused();
  });
});

test('a bind Wind ignores shows struck through so it can be removed', async ({ page }) => {
  await mockHost(page, { values: { inspect: 'a,ctrl+opt+i' } });
  await open(page);
  const r = row(page, 'inspect');
  await expect(r.locator('.kchg .kc').first()).toHaveClass(/bad/);
  await expect(r.locator('.kchg').first()).toHaveAttribute('aria-label', /not used/);
});
