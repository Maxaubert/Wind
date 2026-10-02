import { test, expect } from '@playwright/test';

// Interaction polish (#303): no text selection, no focus box on mouse click, calm motion that
// switches off under prefers-reduced-motion.
test.beforeEach(async ({ page }) => {
  await page.addInitScript(() => {
    const live = { maxLevel: '12', model: 'hybrid', uiTheme: 'dark' };
    const saved = { ...live };
    const listeners = new Set();
    const send = (data) => listeners.forEach((fn) => fn({ data }));
    const cfg = () => ({ type: 'config', values: { ...live }, saved: { ...saved },
                         profiles: { names: ['Default'], active: 'Default' } });
    window.chrome = { webview: {
      addEventListener: (_e, fn) => listeners.add(fn),
      postMessage: (msg) => {
        if (msg.type === 'getConfig') send(cfg());
        else if (msg.type === 'setConfig') live[msg.key] = msg.value;
        else if (msg.type === 'saveSession') { Object.assign(saved, live); send({ type: 'sessionSaved', ok: true }); }
        else if (msg.type === 'mpoState') send({ type: 'mpoState', disabled: false, bootKnown: true, atBoot: false });
      },
    } };
  });
});

const css = (loc, prop) => loc.evaluate((el, p) => getComputedStyle(el).getPropertyValue(p), prop);
const noRing = async (loc) => {
  const style = await css(loc, 'outline-style'), width = await css(loc, 'outline-width');
  expect(style === 'none' || parseFloat(width) === 0).toBe(true);
};
const hasRing = async (loc) => {
  expect(await css(loc, 'outline-style')).not.toBe('none');
  expect(parseFloat(await css(loc, 'outline-width'))).toBeGreaterThan(0);
};

test('text cannot be selected, except inside the search field', async ({ page }) => {
  await page.goto('/');
  const label = page.locator('.row .label').first();
  await expect(label).toBeVisible();
  expect(await css(label, 'user-select')).toBe('none');
  expect(await css(page.locator('.row .desc').first(), 'user-select')).toBe('none');
  expect(await css(page.locator('.side'), 'user-select')).toBe('none');
  expect(await css(page.locator('.side .it').first(), 'user-select')).toBe('none');
  await label.dblclick();
  expect(await page.evaluate(() => String(getSelection()))).toBe('');
  const search = page.getByPlaceholder('Search settings');
  expect(await css(search, 'user-select')).toBe('text');
  await search.fill('zoom level');
  await search.selectText();
  expect(await page.evaluate(() => { const i = document.activeElement; return i.value.slice(i.selectionStart, i.selectionEnd); })).toBe('zoom level');
});

test('a mouse click leaves no focus ring on any control; Tab shows one', async ({ page }) => {
  await page.goto('/');
  const zoom = {
    slider: page.locator('[data-key="maxLevel"] input[type=range]'),
    toggle: page.locator('[role=switch]').first(),
    keycap: page.locator('button.keycap').first(),
    chip: page.locator('.row button.chip').first(),
    sidebar: page.locator('.side .it').nth(2),
  };
  await page.locator('.side .it').first().click();
  for (const [name, loc] of Object.entries(zoom)) {
    if (!(await loc.count())) continue;
    await loc.scrollIntoViewIfNeeded();
    if (name === 'toggle') { await loc.click({ force: true }); await noRing(loc.locator('xpath=following-sibling::*[1]')); continue; }
    await loc.click();
    await noRing(loc);
    if (name === 'keycap') await page.keyboard.press('Escape');   // leave capture (a key press is keyboard use)
  }
  // A select: open it with the mouse, close it, no ring; reach it with the keyboard, ring.
  await page.locator('.side .it[data-g="move"]').click();
  const sel = page.locator('[data-key="mouseAlign"] select');
  await sel.scrollIntoViewIfNeeded();
  await sel.click();
  await noRing(sel);
  await page.keyboard.press('Escape');
  await page.keyboard.press('Shift+Tab'); await page.keyboard.press('Tab');
  await hasRing(sel);
});

test('reduced motion: the capsule and a dialog carry no transition or animation', async ({ page }) => {
  await page.emulateMedia({ reducedMotion: 'reduce' });
  await page.goto('/');
  const slider = page.locator('[data-key="maxLevel"] input[type=range]');
  await slider.focus();
  await page.keyboard.press('ArrowRight');
  const cap = page.locator('.capsule');
  await expect(cap).toBeVisible();
  expect(await cap.evaluate((el) => el.getAnimations().length)).toBe(0);
  const tiny = (v) => v.split(',').every((d) => parseFloat(d) < 0.001);
  expect(tiny(await css(cap, 'animation-duration'))).toBe(true);
  expect(tiny(await css(cap.getByRole('button', { name: 'Save' }), 'transition-duration'))).toBe(true);
  await page.keyboard.press('Control+q');
  const box = page.locator('.mbox');
  await expect(box).toBeVisible();
  expect(tiny(await css(box, 'animation-duration'))).toBe(true);
});

test('normal motion: the capsule eases in and the dialog pops, both under 200 ms', async ({ page }) => {
  await page.emulateMedia({ reducedMotion: 'no-preference' });
  await page.goto('/');
  const t = await page.evaluate(() => [getComputedStyle(document.documentElement).getPropertyValue('--dur-fast').trim(),
    getComputedStyle(document.documentElement).getPropertyValue('--dur').trim()]);
  expect(t).toEqual(['120ms', '180ms']);
  await page.locator('[data-key="maxLevel"] input[type=range]').focus();
  await page.keyboard.press('ArrowRight');
  await page.keyboard.press('Control+q');
  const box = page.locator('.mbox');
  await expect(box).toBeVisible();
  expect(parseFloat(await css(box, 'animation-duration'))).toBeCloseTo(0.18, 2);
});
