// Settings window against the mock bridge: groups and navigation, instant apply, theme, keybind
// rules and the pages that carry the old per-feature rows (redesign, #303).
import { test, expect } from '@playwright/test';

test.beforeEach(async ({ page }) => {
  await page.addInitScript(() => {
    window.__skipSplash = true;
    window.__msgs = [];
    // Built on the first getConfig so a test's own init script (window.__cfgExtra) is already set.
    const init = () => {
      if (window.__live) return;
      window.__live = { maxLevel: '12', zoomInVk: '33', zoomInButton: '2', zoomOutVk: '114',
        cursorLockVk: '113', model: 'hybrid', uiTheme: 'dark', ...(window.__cfgExtra || {}) };
      window.__saved = { ...window.__live };
    };
    const listeners = new Set();
    const send = (data) => listeners.forEach((fn) => fn({ data }));
    window.__hostSend = send;
    window.chrome = { webview: {
      addEventListener: (_e, fn) => listeners.add(fn),
      postMessage: (msg) => {
        window.__msgs.push(msg);
        if (msg.type === 'getConfig') { init(); send({ type: 'config', values: { ...window.__live }, saved: { ...window.__saved },
          profiles: { names: ['Default', 'Gaming'], active: 'Default' } }); }
        else if (msg.type === 'setConfig') window.__live[msg.key] = msg.value;
        else if (msg.type === 'setConfigPersist') { window.__live[msg.key] = msg.value; window.__saved[msg.key] = msg.value; }
        else if (msg.type === 'mpoState') send({ type: 'mpoState', disabled: false, bootKnown: true, atBoot: false });
        else if (msg.type === 'pickExe') send({ type: 'exePicked', name: 'RDR2.exe' });
      },
    } };
  });
});

const sent = (page, type) => page.evaluate((t) => window.__msgs.filter((m) => m.type === t), type);
const go = (page, g) => page.locator('.side .it[data-g="' + g + '"]').click();
const css = (loc, prop) => loc.evaluate((el, p) => getComputedStyle(el).getPropertyValue(p), prop);
const key = (page, k) => page.locator('[data-key="' + k + '"]');

test('sidebar lists the task groups and each one opens its own page', async ({ page }) => {
  await page.goto('/');
  await expect(page.locator('h1')).toHaveText('Zoom');
  await expect(page.locator('.side nav .it')).toHaveText([/Zoom/, /Moving around/, /Cursor/, /Follow typing/, /Colour/, /General/]);
  await expect(page.locator('.side .adv .it')).toHaveText([/Advanced/, /About/]);
  await expect(page.locator('.side .it.sel')).toContainText('Zoom');
  for (const [g, title] of [['move', 'Moving around'], ['cursor', 'Cursor'], ['typing', 'Follow typing'], ['colour', 'Colour'],
    ['general', 'General'], ['advanced', 'Advanced'], ['about', 'About'], ['zoom', 'Zoom']]) {
    await go(page, g);
    await expect(page.locator('h1')).toHaveText(title);
    await expect(page.locator('.side .it.sel')).toHaveAttribute('data-g', g);
    await expect(page.locator('.side .it.sel')).toHaveAttribute('aria-current', 'page');
  }
});

test('pages show their cards', async ({ page }) => {
  await page.goto('/');
  await expect(page.locator('main .card')).toHaveCount(3);
  await go(page, 'move');
  await expect(page.locator('main .card')).toHaveCount(2);
  await go(page, 'general');
  await expect(page.locator('main .card')).toHaveCount(3);
});

test('About: Star on GitHub asks the host to open the repo', async ({ page }) => {
  await page.goto('/');
  await go(page, 'about');
  await page.getByRole('button', { name: 'Star on GitHub' }).click();
  expect(await sent(page, 'openRepo')).toHaveLength(1);
});

test('theme: the title bar cycles dark, light, auto and writes uiTheme each time', async ({ page }) => {
  await page.goto('/');
  const app = page.locator('.wnd');
  const btn = page.locator('[data-theme-cycle]');
  await expect(app).toHaveAttribute('data-theme', 'dark');
  await expect(btn).toHaveAttribute('aria-label', 'Theme: Dark');
  await btn.click();
  await expect(app).toHaveAttribute('data-theme', 'light');
  await expect(btn).toHaveAttribute('aria-label', 'Theme: Light');
  await btn.click();
  await expect(btn).toHaveAttribute('aria-label', 'Theme: Auto');
  await btn.click();
  await expect(app).toHaveAttribute('data-theme', 'dark');
  const writes = (await sent(page, 'setConfig')).filter((m) => m.key === 'uiTheme').map((m) => m.value);
  expect(writes).toEqual(['light', 'auto', 'dark']);
});

test('theme: the General radio group follows the same mode, and a theme change is never unsaved', async ({ page }) => {
  await page.goto('/');
  await go(page, 'general');
  await key(page, '__theme').getByRole('radio', { name: 'Light' }).click();
  await expect(page.locator('.wnd')).toHaveAttribute('data-theme', 'light');
  await expect(page.locator('[data-theme-cycle]')).toHaveAttribute('aria-label', 'Theme: Light');
  await expect(page.locator('.capsule')).toHaveCount(0);
});

test('light theme: shell surfaces and the capsule read on white', async ({ page }) => {
  await page.addInitScript(() => { window.__cfgExtra = { uiTheme: 'light' }; });
  await page.goto('/');
  await expect(page.locator('.wnd')).toHaveAttribute('data-theme', 'light');
  expect(await css(page.locator('.card').first(), 'background-color')).toBe('rgb(255, 255, 255)');
  expect(await css(page.locator('.side'), 'background-color')).toBe('rgb(243, 243, 244)');
  expect(await css(page.locator('.side .it.sel'), 'background-color')).toBe('rgb(228, 228, 228)');
  expect(await css(page.locator('.banner'), 'background-color')).toBe('rgb(246, 246, 247)');
  await key(page, 'maxLevel').locator('input[type=range]').fill('20');
  const cap = page.locator('.capsule');
  await expect(cap).toBeVisible();
  const [bg, fg] = await cap.evaluate((el) => [getComputedStyle(el).backgroundColor, getComputedStyle(el).color]);
  expect(bg).not.toBe(fg);
  expect(bg).not.toBe('rgba(0, 0, 0, 0)');
});

test('focus order: Tab goes title bar, search, sidebar, then the page, and focus is visible', async ({ page }) => {
  await page.goto('/');
  await expect(page.locator('h1')).toHaveText('Zoom');
  const where = () => page.evaluate(() => {
    const a = document.activeElement;
    if (!a) return '';
    if (a.closest('header.tb')) return 'tb';
    if (a.closest('.side .search')) return 'search';
    if (a.closest('.side')) return 'side';
    if (a.closest('main')) return 'main';
    return a.tagName;
  });
  const seen = [];
  for (let i = 0; i < 40; i++) {
    await page.keyboard.press('Tab');
    const w = await where();
    if (seen.at(-1) !== w) seen.push(w);
  }
  const order = seen.filter((w) => ['tb', 'search', 'side', 'main'].includes(w));
  const uniq = [...new Set(order)];
  expect(uniq).toEqual(expect.arrayContaining(['tb', 'search', 'side', 'main']));
  expect(uniq.indexOf('search')).toBeLessThan(uniq.indexOf('side'));
  expect(uniq.indexOf('side')).toBeLessThan(uniq.indexOf('main'));
  const ring = await page.evaluate(() => {
    const s = getComputedStyle(document.activeElement);
    return s.outlineStyle !== 'none' || s.boxShadow !== 'none';
  });
  expect(ring).toBe(true);
});

test('Advanced: engine row, per-window rows follow Auto, app list writes through the session', async ({ page }) => {
  await page.goto('/');
  await go(page, 'advanced');
  await expect(page.getByRole('button', { name: 'Restart Wind' })).toHaveCount(0);
  await key(page, 'model').locator('select').selectOption('render');
  await expect(page.getByRole('button', { name: 'Restart Wind' })).toBeVisible();
  await expect(key(page, 'engineGame')).toHaveCount(0);
  await key(page, 'model').locator('select').selectOption('hybrid');
  const apps = key(page, 'renderExclude');
  await apps.getByRole('button', { name: /Manage/ }).click();
  await page.getByRole('button', { name: 'Add program...' }).click();
  await page.getByRole('button', { name: 'Add program...' }).click();   // the same exe twice is ignored
  await expect(page.getByRole('button', { name: 'Remove RDR2.exe' })).toHaveCount(1);
  await page.keyboard.press('Escape');
  await expect(page.getByRole('dialog')).toHaveCount(0);
  expect((await sent(page, 'setConfig')).some((m) => m.key === 'renderExclude' && m.value.includes('RDR2.exe'))).toBe(true);
});

test('Typing page: caret on, focus off by default; a toggle writes at once', async ({ page }) => {
  await page.goto('/');
  await go(page, 'typing');
  await expect(key(page, 'trackCaret').getByRole('switch')).toBeChecked();
  await expect(key(page, 'trackFocus').getByRole('switch')).not.toBeChecked();
  await key(page, 'trackFocus').getByRole('switch').check({ force: true });
  expect((await sent(page, 'setConfig')).some((m) => m.key === 'trackFocus' && String(m.value) === '1')).toBe(true);
  await expect(page.locator('.capsule')).toContainText('1 unsaved change');
});

test('Colour page: only warmth and brightness, neutral by default (no warmth, full brightness)', async ({ page }) => {
  await page.goto('/');
  await go(page, 'colour');
  await expect(page.locator('main input[type=range]')).toHaveCount(2);
  await expect(key(page, 'colorWarmPct').locator('input[type=range]')).toHaveValue('0');
  await expect(key(page, 'colorDimPct').locator('input[type=range]')).toHaveValue('100');
});

test('Move page: pan rows ship unbound and bind with the shared rules', async ({ page }) => {
  await page.goto('/');
  await go(page, 'move');
  const left = key(page, '__panLeft').locator('.keycap').first();
  await expect(left).toHaveText('Add key');
  await left.click();
  await page.keyboard.press('Control+Alt+ArrowLeft');
  expect((await sent(page, 'setConfigPersist')).some((m) => m.key === 'panLeftVk' && m.value === '37')).toBe(true);
});

test('keybind safety: typing keys and system combos are refused and the row keeps listening', async ({ page }) => {
  await page.goto('/');
  const cap = key(page, '__zoomOut').locator('.keycap').first();
  await cap.click();
  await page.keyboard.press('a');
  await expect(cap).toHaveClass(/armed/);
  expect((await sent(page, 'setConfigPersist')).filter((m) => m.key === 'zoomOutVk' && m.value !== '0')).toHaveLength(0);
  await page.keyboard.press('F2');
  await expect(cap).not.toHaveClass(/armed/);
  expect((await sent(page, 'setConfigPersist')).some((m) => m.key === 'zoomOutVk' && m.value === '113')).toBe(true);
});

test('Escape cancels an armed capture and keeps the old binding', async ({ page }) => {
  await page.goto('/');
  const cap = key(page, '__zoomIn').locator('.keycap').first();
  const before = await cap.textContent();
  await cap.click();
  await expect(cap).toHaveClass(/armed/);
  await page.keyboard.press('Escape');
  await expect(cap).not.toHaveClass(/armed/);
  await expect(cap).toHaveText(before);
});

test('a modifier alone never binds; the row keeps listening', async ({ page }) => {
  await page.goto('/');
  const cap = key(page, '__zoomOut').locator('.keycap').first();
  await cap.click();
  await page.keyboard.press('Control');
  await page.keyboard.press('Shift');
  await expect(cap).toHaveClass(/armed/);
  expect((await sent(page, 'setConfigPersist')).filter((m) => m.key === 'zoomOutVk' && m.value !== '0')).toHaveLength(0);
});

test('stored binds the rules refuse are reset once and persisted', async ({ page }) => {
  await page.addInitScript(() => { window.__cfgExtra = { zoomInVk: '65' }; });
  await page.goto('/');
  await expect(page.locator('h1')).toHaveText('Zoom');
  await expect.poll(async () => (await sent(page, 'setConfigPersist')).some((m) => m.key === 'zoomInVk' && m.value === '0')).toBe(true);
  await expect(page.locator('.capsule')).toHaveCount(0);
});

test('no reset when every stored bind is allowed', async ({ page }) => {
  await page.goto('/');
  await expect(page.locator('h1')).toHaveText('Zoom');
  expect(await sent(page, 'setConfigPersist')).toHaveLength(0);
});

test('a settings write the host could not save shows a dialog', async ({ page }) => {
  await page.goto('/');
  await expect(page.locator('h1')).toHaveText('Zoom');
  await page.evaluate(() => window.__hostSend({ type: 'configWriteFailed', key: 'maxLevel' }));
  await expect(page.getByRole('dialog')).toContainText('maxLevel');
});
