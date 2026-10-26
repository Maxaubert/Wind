// Screen-reader and keyboard regression tests (issue #201), rewritten for the redesigned shell (#303).
//
// The original finding: nothing in the settings list had an accessible name, so the accessibility
// tree read "checkbox, checked" with no hint which setting it was. These tests pin that fix and the
// keyboard and focus work around it on the new sidebar, banner, card and capsule shell. Every one of
// them is invisible in a screenshot.
import { test, expect } from '@playwright/test';

const GROUPS = ['hotkeys', 'zoom', 'view', 'prefs', 'tray', 'about'];

test.beforeEach(async ({ page }) => {
  await page.addInitScript(() => {
    window.__msgs = [];
    // Built on the first getConfig so a test's own init script is already set.
    let live, saved;
    const init = () => { if (live) return; live = { maxLevel: '12', zoomInVk: '33', zoomInButton: '2', cursorLockVk: '113', model: 'transform',
      uiTheme: 'dark', trackCaret: '1', noSwallowApps: 'netflix.exe', showAdvanced: '1' };
      saved = { ...live }; };
    const listeners = new Set();
    const send = (data) => listeners.forEach((fn) => fn({ data }));
    window.__hostSend = send;
    window.chrome = { webview: {
      addEventListener: (_e, fn) => listeners.add(fn),
      postMessage: (msg) => {
        window.__msgs.push(msg);
        if (msg.type === 'getConfig') { init(); send({ type: 'config', values: { ...live }, saved: { ...saved },
          profiles: { names: ['Default', 'Gaming'], active: 'Default' } }); }
        else if (msg.type === 'setConfig') live[msg.key] = msg.value;
        else if (msg.type === 'saveSession') { Object.assign(saved, live); send({ type: 'sessionSaved', ok: true }); }
        else if (msg.type === 'mpoState') send({ type: 'mpoState', disabled: false, bootKnown: true, atBoot: false });
      },
    } };
  });
});

const go = (page, g) => page.locator('.side .it[data-g="' + g + '"]').click();

// Accessible name computed from aria-label, aria-labelledby, an associated label or content.
const unnamedIn = (page, root) => page.locator(root).evaluate((r) => {
  const out = [];
  for (const el of r.querySelectorAll('input, button, select, textarea, [role=switch], [role=radio], [role=combobox]')) {
    if (el.closest('[hidden]') || el.getAttribute('aria-hidden') === 'true') continue;
    const byIds = (a) => (el.getAttribute(a) || '').split(/\s+/).filter(Boolean)
      .map((id) => (document.getElementById(id) || {}).innerText || '').join(' ').trim();
    const name = (el.getAttribute('aria-label') || '') || byIds('aria-labelledby') ||
      (el.labels && el.labels[0] ? el.labels[0].innerText : '') || (el.innerText || '').trim() || (el.getAttribute('title') || '');
    if (!name.trim()) out.push(el.tagName.toLowerCase() + (el.type ? '[' + el.type + ']' : ''));
  }
  return out;
});

test('every control on every page has an accessible name', async ({ page }) => {
  await page.goto('/');
  await expect(page.locator('h1')).toHaveText('Hotkeys');
  for (const g of GROUPS) {
    await go(page, g);
    await expect(page.locator('.side .it.sel')).toHaveAttribute('data-g', g);
    expect(await unnamedIn(page, 'main'), g).toEqual([]);
  }
  expect(await unnamedIn(page, '.tb')).toEqual([]);
  expect(await unnamedIn(page, '.side')).toEqual([]);
});

test('a control is named by its own row label', async ({ page }) => {
  await page.goto('/');
  await go(page, 'zoom');
  await expect(page.getByRole('slider', { name: /Max zoom/i })).toBeVisible();
  await go(page, 'view');
  await expect(page.getByRole('switch', { name: /text cursor/i }).first()).toBeVisible();
  await go(page, 'zoom');
  await expect(page.getByRole('combobox', { name: /engine/i }).first()).toBeVisible();
  await go(page, 'hotkeys');
  await expect(page.getByRole('switch', { name: 'Hide pointer' })).toBeVisible();   // an extra key's on/off switch
});

test('row descriptions are linked to their control, not orphaned', async ({ page }) => {
  await page.goto('/');
  const broken = await page.locator('main').evaluate((m) => [...m.querySelectorAll('[aria-describedby]')]
    .flatMap((el) => el.getAttribute('aria-describedby').split(/\s+/).filter(Boolean)
      .filter((id) => !document.getElementById(id)).map((id) => id)));
  expect(broken).toEqual([]);
  expect(await page.locator('main [aria-describedby]').count()).toBeGreaterThan(0);
});

test('sliders speak their unit instead of a bare number', async ({ page }) => {
  await page.goto('/');
  await go(page, 'zoom');
  await expect(page.locator('[data-key="maxLevel"] input[type=range]')).toHaveAttribute('aria-valuetext', '12 times');
});

test('focus is visible', async ({ page }) => {
  {
    await page.goto('/');
    await page.keyboard.press('Tab');   // keyboard modality: the ring is for keyboard users only
    for (const sel of ['.side .it', '.side .search input', '.tb .wc', 'main .kchg']) {
      await page.locator(sel).first().focus();
      const ring = await page.locator(sel).first().evaluate((el) => {
        const s = getComputedStyle(el);
        const box = el.closest('.search') ? getComputedStyle(el.closest('.search')).borderTopColor : '';
        return { outline: s.outlineStyle !== 'none' && parseFloat(s.outlineWidth) > 0, shadow: s.boxShadow !== 'none', box };
      });
      expect(ring.outline || ring.shadow || ring.box !== '', sel).toBe(true);
    }
  }
});

test('a dialog takes focus, traps Tab, and gives focus back on close', async ({ page }) => {
  await page.goto('/');
  const manage = page.locator('[data-key="noSwallowApps"]').getByRole('button', { name: /Manage/ });
  await manage.focus();
  await page.keyboard.press('Enter');
  const dlg = page.getByRole('dialog');
  await expect(dlg).toBeVisible();
  expect(await page.evaluate(() => !!document.activeElement.closest('[role=dialog]'))).toBe(true);
  for (let i = 0; i < 8; i++) {
    await page.keyboard.press('Tab');
    expect(await page.evaluate(() => !!document.activeElement.closest('[role=dialog]'))).toBe(true);
  }
  await page.keyboard.press('Escape');
  await expect(dlg).toHaveCount(0);
  await expect(manage).toBeFocused();
});

test('the unsaved-changes prompt announces itself as a labelled modal dialog', async ({ page }) => {
  await page.goto('/');
  await go(page, 'zoom');
  await page.locator('[data-key="maxLevel"] input[type=range]').fill('20');
  await page.getByRole('button', { name: 'Close' }).click();
  const dlg = page.getByRole('dialog', { name: 'Unsaved changes' });
  await expect(dlg).toBeVisible();
  await expect(dlg).toHaveAttribute('aria-modal', 'true');
  expect(await page.evaluate(() => !!document.activeElement.closest('[role=dialog]'))).toBe(true);
  await page.keyboard.press('Escape');
  await expect(page.getByRole('dialog')).toHaveCount(0);
});

test('the capsule is reachable by keyboard and its buttons are named', async ({ page }) => {
  await page.goto('/');
  await go(page, 'zoom');
  await page.locator('[data-key="maxLevel"] input[type=range]').fill('20');
  const cap = page.locator('.capsule');
  await expect(cap.getByRole('button', { name: 'Save' })).toBeVisible();
  await expect(cap.getByRole('button', { name: 'Discard' })).toBeVisible();
  await cap.getByRole('button', { name: 'Save' }).focus();
  await page.keyboard.press('Enter');
  await expect(cap).toHaveCount(0);
  expect(await page.evaluate(() => window.__msgs.filter((m) => m.type === 'saveSession').length)).toBe(1);
});

test('the sidebar is a navigation landmark that marks the current page', async ({ page }) => {
  await page.goto('/');
  await expect(page.getByRole('navigation', { name: 'Settings sections' })).toBeVisible();
  await expect(page.locator('.side .it[aria-current=page]')).toHaveCount(1);
  await go(page, 'view');
  await expect(page.locator('.side .it[aria-current=page]')).toHaveAttribute('data-g', 'view');
});

test('the page has a lang, one h1, and hierarchical headings', async ({ page }) => {
  await page.goto('/');
  await expect(page.locator('h1')).toHaveCount(1);
  expect(await page.evaluate(() => document.documentElement.lang)).not.toBe('');
  const levels = await page.evaluate(() => [...document.querySelectorAll('h1,h2,h3,h4')].map((h) => +h.tagName[1]));
  for (let i = 1; i < levels.length; i++) expect(levels[i] - levels[i - 1]).toBeLessThanOrEqual(1);
});

test('decorative icons are hidden from the accessibility tree', async ({ page }) => {
  await page.goto('/');
  const exposed = await page.locator('.wnd').evaluate((r) => [...r.querySelectorAll('.side svg, .tb svg')]
    .filter((s) => !s.closest('[aria-hidden=true]') && s.getAttribute('aria-hidden') !== 'true' && !s.getAttribute('aria-label')
      && !s.closest('button[aria-label]')).length);
  expect(exposed).toBe(0);
});

test('Save is announced through the live region', async ({ page }) => {
  await page.goto('/');
  await go(page, 'zoom');
  await page.locator('[data-key="maxLevel"] input[type=range]').fill('20');
  await page.locator('.capsule').getByRole('button', { name: 'Save' }).click();
  await expect(page.locator('[role=status][aria-live=polite]')).toContainText('Settings saved');
});

test('Tab escapes an armed keybind capture instead of binding Tab', async ({ page }) => {
  await page.goto('/');
  const cap = page.locator('[data-key="__zoomIn"] .kchg').first();
  await cap.focus();
  await page.keyboard.press('Enter');
  await expect(cap).toHaveAccessibleName(/Recording/);
  await page.keyboard.press('Tab');
  await expect(cap).toHaveAccessibleName(/Change/);
  const bound = await page.evaluate(() => window.__msgs.some((m) => /^setConfig/.test(m.type) && m.value === '9'));
  expect(bound).toBe(false);
});

test('an armed keycap says so and carries its instructions as a description', async ({ page }) => {
  await page.goto('/');
  const cap = page.locator('[data-key="__zoomIn"] .kchg').first();
  await cap.click();
  await expect(cap).toHaveAccessibleName(/Recording/);
  const desc = await cap.evaluate((el) => (el.getAttribute('aria-describedby') || '').split(/\s+/)
    .map((id) => (document.getElementById(id) || {}).textContent || '').join(' '));
  expect(desc).toMatch(/Escape cancels/i);
  await expect(page.locator('[data-key="__zoomIn"] [role=status]')).toContainText(/Listening/);
});

test('the theme picker is a radio group with arrow-key navigation', async ({ page }) => {
  await page.goto('/');
  await go(page, 'prefs');
  const group = page.locator('[data-key="uiPalette"]').getByRole('radiogroup');
  await expect(group.getByRole('radio')).toHaveCount(4);
  await group.getByRole('radio', { checked: true }).focus();
  await page.keyboard.press('ArrowRight');
  await expect(group.getByRole('radio', { checked: true })).toHaveCount(1);
  expect((await page.evaluate(() => window.__msgs.filter((m) => m.type === 'setConfig' && m.key === 'uiPalette'))).length).toBe(1);
});

test('the profile selector has an accessible name and the search box is a labelled search field', async ({ page }) => {
  await page.goto('/');
  await expect(page.getByRole('searchbox', { name: 'Search settings' })).toBeVisible();
  await go(page, 'prefs');
  await expect(page.locator('[data-key="__profiles"] .trig')).toHaveAccessibleName(/profile/i);
});

// ---- #514: NVDA read Settings as "an overlaid window" ----------------------------------------------

test('while a prompt is open the rest of the app is inert, and it comes back on close', async ({ page }) => {
  await page.goto('/');
  await go(page, 'zoom');
  await page.locator('[data-key="maxLevel"] input[type=range]').fill('20');
  const closeBtn = page.locator('.tb').getByRole('button', { name: 'Close' });
  await closeBtn.click();
  const dlg = page.getByRole('dialog', { name: 'Unsaved changes' });
  await expect(dlg).toBeVisible();
  // Every app layer behind the prompt is inert; the live region stays live for announcements.
  for (const sel of ['.body', '.tb', '.capsule']) await expect(page.locator(sel)).toHaveJSProperty('inert', true);
  await expect(page.locator('.wnd > [role=status][aria-live=polite]')).toHaveJSProperty('inert', false);
  expect(await dlg.evaluate((d) => !!d.closest('[inert]'))).toBe(false);
  // Chromium's own accessibility tree (what NVDA reads) holds only the dialog: no page landmark,
  // sidebar, window button or capsule button.
  const cdp = await page.context().newCDPSession(page);
  const { nodes } = await cdp.send('Accessibility.getFullAXTree');
  const shown = nodes.filter((n) => !n.ignored).map((n) => [n.role && n.role.value, n.name && n.name.value]);
  expect(shown.filter(([r]) => r === 'main' || r === 'navigation')).toEqual([]);
  expect(shown.filter(([r]) => r === 'button').map(([, nm]) => nm).sort())
    .toEqual(['Discard', 'Keep for this session', 'Save']);
  await page.keyboard.press('Escape');
  await expect(dlg).toHaveCount(0);
  expect(await page.evaluate(() => document.querySelectorAll('[inert]').length)).toBe(0);
  await expect(closeBtn).toBeFocused();
});

test('a dialog inside the page (app list) inerts the rest of the app but not itself', async ({ page }) => {
  await page.goto('/');
  await page.locator('[data-key="noSwallowApps"]').getByRole('button', { name: /Manage/ }).click();
  const dlg = page.getByRole('dialog');
  await expect(dlg).toBeVisible();
  expect(await dlg.evaluate((d) => !!d.closest('[inert]'))).toBe(false);
  await expect(page.locator('.side')).toHaveJSProperty('inert', true);
  await expect(page.locator('.tb')).toHaveJSProperty('inert', true);
  await expect(page.locator('main .banner')).toHaveJSProperty('inert', true);
  await expect(dlg.getByRole('button', { name: 'Add program...' })).toBeFocused();
  await page.keyboard.press('Escape');
  await expect(dlg).toHaveCount(0);
  expect(await page.evaluate(() => document.querySelectorAll('[inert]').length)).toBe(0);
});

test('the save capsule is a labelled region, not a live region, and the first change is announced once', async ({ page }) => {
  await page.goto('/');
  await go(page, 'zoom');
  const live = page.locator('.wnd > [role=status][aria-live=polite]');
  await page.locator('[data-key="maxLevel"] input[type=range]').fill('20');
  const cap = page.locator('.capsule');
  await expect(cap).toBeVisible();
  expect(await cap.getAttribute('role')).toBe('region');
  expect(await cap.getAttribute('aria-live')).toBeNull();
  expect(await cap.evaluate((c) => !!c.querySelector('[role=status], [aria-live]'))).toBe(false);
  await expect(page.getByRole('region', { name: 'Unsaved changes' })).toBeVisible();
  await expect(live).toContainText('Unsaved changes');
  // A second change does not repeat it.
  await page.evaluate(() => { document.querySelector('.wnd > [role=status]').textContent = ''; });
  await page.locator('[data-key="maxLevel"] input[type=range]').fill('18');
  await expect(cap).toContainText('1 unsaved change');
  await expect(live).toHaveText('');
});

test('the title bar is no banner landmark and follows the page in the DOM', async ({ page }) => {
  await page.goto('/');
  await expect(page.locator('h1')).toHaveText('Hotkeys');
  await expect(page.getByRole('banner')).toHaveCount(0);
  const after = await page.evaluate(() => {
    const tb = document.querySelector('.tb'), main = document.querySelector('main');
    return !!(main.compareDocumentPosition(tb) & Node.DOCUMENT_POSITION_FOLLOWING);
  });
  expect(after).toBe(true);
  // Drawn on top all the same.
  const y = await page.evaluate(() => [document.querySelector('.tb').getBoundingClientRect().top,
                                       document.querySelector('.body').getBoundingClientRect().top]);
  expect(y[0]).toBe(0);
  expect(y[1]).toBeGreaterThanOrEqual(38);
  await expect(page.locator('.tb .brand')).toHaveAttribute('aria-hidden', 'true');
});

test('main is named "Search results" while searching', async ({ page }) => {
  await page.goto('/');
  await page.locator('.side .search input').fill('zoom');
  await expect(page.locator('main')).toHaveAttribute('aria-label', 'Search results');
});
