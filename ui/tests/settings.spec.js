// Settings window against the mock bridge: the seven pages and navigation, instant apply, the
// advanced switch and the Hotkeys page (one-box bindings, wheel capture, limits, extra-key switches).
// Redesign #303, structure and hotkeys #318.
import { test, expect } from '@playwright/test';

test.beforeEach(async ({ page }) => {
  await page.addInitScript(() => {
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
// The last value a keybind capture persisted for an ini key (undefined when it never wrote it).
const persisted = async (page, k) => (await sent(page, 'setConfigPersist')).filter((m) => m.key === k).at(-1)?.value;
const advanced = async (page, on) => {
  await go(page, 'prefs');
  const sw = key(page, 'showAdvanced').getByRole('switch');
  if (on) await sw.check({ force: true }); else await sw.uncheck({ force: true });
};

// ---- structure ------------------------------------------------------------------------------------

test('Settings opens on Hotkeys; the sidebar is four pages, a divider, then three, with no group labels', async ({ page }) => {
  await page.goto('/');
  await expect(page.locator('h1')).toHaveText('Hotkeys');
  await expect(page.locator('.side .it.sel')).toContainText('Hotkeys');
  await expect(page.locator('.side nav:not(.bottom) .it')).toHaveText([/Hotkeys/, /Zoom/, /View/]);
  await expect(page.locator('.side nav.bottom .it')).toHaveText([/Preferences/, /Tray menu/, /About/]);
  await expect(page.locator('.side .lbl')).toHaveCount(0);
  await expect(page.locator('.side .it[data-g="advanced"]')).toHaveCount(0);   // no Advanced tab
  for (const [g, title] of [['zoom', 'Zoom'], ['view', 'View'], ['prefs', 'Preferences'],
    ['tray', 'Tray menu'], ['about', 'About'], ['hotkeys', 'Hotkeys']]) {
    await go(page, g);
    await expect(page.locator('h1')).toHaveText(title);
    await expect(page.locator('.side .it.sel')).toHaveAttribute('data-g', g);
    await expect(page.locator('.side .it.sel')).toHaveAttribute('aria-current', 'page');
  }
});

test('pages show their sections; a section with nothing to show is dropped', async ({ page }) => {
  await page.goto('/');
  await expect(page.locator('main .card')).toHaveCount(2);                // Zoom, Extra keys
  await expect(page.locator('main .cap')).toHaveText(['Zoom', 'Extra keys']);
  await go(page, 'zoom');
  await expect(page.locator('main .cap')).toHaveText(['Level and speed']);   // Easing and Engine are advanced
  await go(page, 'view');
  await expect(page.locator('main .cap')).toHaveText(['Speed', 'Pointer', 'Typing and focus']);
  await go(page, 'prefs');
  await expect(page.locator('main .cap')).toHaveText(['General', 'Screen light', 'Troubleshooting']);
  // Never fewer than two rows in a section while the advanced switch is off.
  for (const g of ['hotkeys', 'zoom', 'view', 'prefs']) {
    await go(page, g);
    const counts = await page.locator('main .card').evaluateAll((cs) => cs.map((c) => c.querySelectorAll('.row').length));
    for (const n of counts) expect(n, g).toBeGreaterThanOrEqual(2);
  }
});

test('copy: the row names and descriptions follow the table, and "Never use Render for" is gone', async ({ page }) => {
  await page.goto('/');
  await expect(page.locator('[data-key="__zoomIn"] .label')).toHaveText('Zoom in');
  await expect(page.locator('[data-key="__zoomIn"] .desc')).toHaveText('Hold or scroll to magnify the view.');
  await expect(page.locator('[data-key="__hideCursor"] .label')).toHaveText('Hide pointer');
  await expect(page.locator('[data-key="__pan"] .desc')).toHaveText('Hold the modifiers and press an arrow key to move the view.');
  await advanced(page, true);
  await go(page, 'zoom');
  await expect(page.locator('main .cap')).toHaveText(['Level and speed', 'Easing', 'Engine']);
  await expect(page.locator('[data-key="smoothZoomAccel"] .label')).toHaveText('Soft start');
  await expect(page.locator('[data-key="engineGame"] .desc')).toHaveText('Full-screen and borderless games.');
  await expect(page.getByText('Never use Render for')).toHaveCount(0);
  await expect(key(page, 'renderExclude')).toHaveCount(0);
});

test('About: Star on GitHub asks the host to open the repo', async ({ page }) => {
  await page.goto('/');
  await go(page, 'about');
  await page.getByRole('button', { name: 'Star on GitHub' }).click();
  expect(await sent(page, 'openRepo')).toHaveLength(1);
});

test('the title bar has only minimize, maximize and close; Maximize asks the host', async ({ page }) => {
  await page.goto('/');
  await expect(page.locator('header.tb button')).toHaveCount(3);
  await expect(page.locator('[data-theme-cycle]')).toHaveCount(0);
  await expect(page.getByRole('button', { name: /^Theme:/ })).toHaveCount(0);
  await page.getByRole('button', { name: 'Maximize' }).click();
  expect((await sent(page, 'window')).map((m) => m.action)).toEqual(['maximize']);
  await page.evaluate(() => window.__hostSend({ type: 'windowState', maximized: true }));
  await expect(page.getByRole('button', { name: 'Restore' })).toBeVisible();
});

// ---- the advanced switch --------------------------------------------------------------------------

test('Show advanced settings reveals the advanced rows inline, with no marker, and hides them again', async ({ page }) => {
  await page.goto('/');
  await expect(key(page, 'noSwallowApps')).toHaveCount(0);
  await go(page, 'zoom');
  for (const k of ['model', 'engineGame', 'smoothZoomAccel', 'smoothZoomRamp']) await expect(key(page, k)).toHaveCount(0);
  await go(page, 'view');
  await expect(key(page, 'lockApps')).toHaveCount(0);
  await expect(key(page, 'panGlideMaxPx')).toBeVisible();   // Pan glide is a plain row (#430)
  await expect(key(page, 'mouseAlign')).toBeVisible();
  // Troubleshooting is always there; the switch itself is a global key, so it never raises the capsule.
  await go(page, 'prefs');
  await expect(key(page, 'diagnostics')).toBeVisible();
  await expect(key(page, 'showAdvanced').getByRole('switch')).not.toBeChecked();
  await key(page, 'showAdvanced').getByRole('switch').check({ force: true });
  expect((await sent(page, 'setConfig')).some((m) => m.key === 'showAdvanced' && String(m.value) === '1')).toBe(true);
  await expect(page.locator('.capsule')).toHaveCount(0);
  await go(page, 'zoom');
  for (const k of ['model', 'engineGame', 'smoothZoomAccel', 'smoothZoomRamp']) await expect(key(page, k)).toBeVisible();
  await expect(page.locator('main .cap')).toHaveText(['Level and speed', 'Easing', 'Engine']);
  await go(page, 'view');
  await expect(key(page, 'lockApps')).toBeVisible();
  await go(page, 'hotkeys');
  await expect(key(page, 'noSwallowApps')).toBeVisible();
  // No marker: an advanced row reads exactly like a plain one.
  await expect(key(page, 'noSwallowApps').locator('.label')).toHaveText('Share zoom keys with these apps');
  await expect(key(page, 'noSwallowApps').locator('.label *')).toHaveCount(0);
  await advanced(page, false);
  await go(page, 'hotkeys');
  await expect(key(page, 'noSwallowApps')).toHaveCount(0);
});

test('an advanced switch left on in the ini shows the advanced rows at launch', async ({ page }) => {
  await page.addInitScript(() => { window.__cfgExtra = { showAdvanced: '1' }; });
  await page.goto('/');
  await expect(key(page, 'noSwallowApps')).toBeVisible();
  await go(page, 'prefs');
  await expect(key(page, 'showAdvanced').getByRole('switch')).toBeChecked();
});

test('Zoom engine: the engine row, per-window rows follow Auto, the share-keys app list writes through the session', async ({ page }) => {
  await page.addInitScript(() => { window.__cfgExtra = { showAdvanced: '1' }; });
  await page.goto('/');
  await go(page, 'zoom');
  await expect(page.getByRole('button', { name: 'Restart Wind' })).toHaveCount(0);
  await key(page, 'model').locator('select').selectOption('render');
  await expect(page.getByRole('button', { name: 'Restart Wind' })).toBeVisible();
  await expect(key(page, 'engineGame')).toHaveCount(0);
  await key(page, 'model').locator('select').selectOption('hybrid');
  await expect(key(page, 'engineGame')).toBeVisible();
  await go(page, 'hotkeys');
  const apps = key(page, 'noSwallowApps');
  await apps.getByRole('button', { name: /Manage/ }).click();
  await page.getByRole('button', { name: 'Add program...' }).click();
  await page.getByRole('button', { name: 'Add program...' }).click();   // the same exe twice is ignored
  await expect(page.getByRole('button', { name: 'Remove RDR2.exe' })).toHaveCount(1);
  await page.keyboard.press('Escape');
  await expect(page.getByRole('dialog')).toHaveCount(0);
  expect((await sent(page, 'setConfig')).some((m) => m.key === 'noSwallowApps' && m.value.includes('RDR2.exe'))).toBe(true);
});

// ---- other pages -----------------------------------------------------------------------------------

test('View: the edge margin shows only when the pointer is kept within the edges', async ({ page }) => {
  await page.goto('/');
  await go(page, 'view');
  await expect(key(page, 'mouseMarginPct')).toHaveCount(0);
  await key(page, 'mouseAlign').locator('select').selectOption('1');
  await expect(key(page, 'mouseMarginPct')).toBeVisible();
});

test('High resolution cursor is tagged Experimental and turns the release glide off (#427)', async ({ page }) => {
  await page.goto('/');
  await go(page, 'view');
  await expect(key(page, 'txSamplingMode').locator('.tag')).toHaveText('Experimental');
  await go(page, 'zoom');
  await expect(key(page, 'zoomEaseOutMs')).not.toHaveClass(/disabled/);
  await go(page, 'view');
  await key(page, 'txSamplingMode').getByRole('switch').check({ force: true });
  await go(page, 'zoom');
  await expect(key(page, 'zoomEaseOutMs')).toHaveClass(/disabled/);
  await expect(key(page, 'zoomEaseOutMs').locator('input[type=range]')).toBeDisabled();
});

test('Typing and focus: caret on, focus off by default; a toggle writes at once', async ({ page }) => {
  await page.goto('/');
  await go(page, 'view');
  await expect(key(page, 'trackCaret').getByRole('switch')).toBeChecked();
  await expect(key(page, 'trackFocus').getByRole('switch')).not.toBeChecked();
  await key(page, 'trackFocus').getByRole('switch').check({ force: true });
  expect((await sent(page, 'setConfig')).some((m) => m.key === 'trackFocus' && String(m.value) === '1')).toBe(true);
  await expect(page.locator('.capsule')).toContainText('1 unsaved change');
});

test('Screen light (in Preferences): warmth and brightness, neutral by default (no warmth, full brightness)', async ({ page }) => {
  await page.goto('/');
  await go(page, 'prefs');
  await expect(page.locator('main input[type=range]')).toHaveCount(2);
  await expect(key(page, 'colorWarmPct').locator('input[type=range]')).toHaveValue('0');
  await expect(key(page, 'colorDimPct').locator('input[type=range]')).toHaveValue('100');
});

test('the save capsule reads on the dark surfaces', async ({ page }) => {
  await page.goto('/');
  await go(page, 'zoom');
  await key(page, 'maxLevel').locator('input[type=range]').fill('20');
  const cap = page.locator('.capsule');
  await expect(cap).toBeVisible();
  const [bg, fg] = await cap.evaluate((el) => [getComputedStyle(el).backgroundColor, getComputedStyle(el).color]);
  expect(bg).not.toBe(fg);
  expect(bg).not.toBe('rgba(0, 0, 0, 0)');
});

test('focus order: Tab goes title bar, search, sidebar, then the page, and focus is visible', async ({ page }) => {
  await page.goto('/');
  await expect(page.locator('h1')).toHaveText('Hotkeys');
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

// ---- Hotkeys: bindings ----------------------------------------------------------------------------

const boxes = (page, k) => key(page, k).locator('.kb .kc');

test('Hotkeys: one box per binding with the plus inside, "or" between two, a ghost to add the next', async ({ page }) => {
  await page.goto('/');
  // Zoom in holds Mouse 5 and PageUp (the old one-slot form): two boxes, no room for a third.
  await expect(boxes(page, '__zoomIn')).toHaveText(['Mouse 5', 'PageUp']);
  await expect(key(page, '__zoomIn').locator('.pl')).toHaveText('or');
  await expect(key(page, '__zoomIn').locator('.kc.ghost')).toHaveCount(0);
  // Zoom out holds F3: one box and a ghost, because a zoom row takes two.
  await expect(boxes(page, '__zoomOut')).toHaveText(['F3']);
  await expect(key(page, '__zoomOut').locator('.kc.ghost')).toHaveText('Add key');
  // Inspect mode: one box, one binding only, so no ghost.
  await expect(boxes(page, '__cursorLock')).toHaveText(['F2']);
  await expect(key(page, '__cursorLock').locator('.kc.ghost')).toHaveCount(0);
  // Hide pointer is unbound: a ghost that says Set key.
  await expect(boxes(page, '__hideCursor')).toHaveCount(0);
  await expect(key(page, '__hideCursor').locator('.kc.ghost')).toHaveText('Set key');
});

test('Hotkeys: a combo is ONE box with the plus inside it', async ({ page }) => {
  await page.addInitScript(() => { window.__cfgExtra = { zoomOutVk: '113', zoomOutMods: '5', hideCursorVk: '119', hideCursorMods: '3' }; });
  await page.goto('/');
  const box = boxes(page, '__zoomOut');
  await expect(box).toHaveCount(1);
  await expect(box).toHaveText('Ctrl+Shift+F2');
  await expect(box.locator('.kp')).toHaveText(['+', '+']);
  await expect(boxes(page, '__hideCursor')).toHaveText('Ctrl+Alt+F8');
});

test('Hotkeys: click a binding to record it again; Escape puts the old one back', async ({ page }) => {
  await page.goto('/');
  const btn = key(page, '__zoomIn').locator('.kchg').first();   // Mouse 5
  await btn.click();
  await expect(btn).toHaveAccessibleName(/Recording/);
  await expect(key(page, '__zoomIn').locator('.kc.live')).toHaveText('Press keys...');
  // The old binding is cleared at once so the core stops swallowing it.
  expect(await persisted(page, 'zoomInButton')).toBe('0');
  await page.keyboard.press('Escape');
  await expect(btn).toHaveAccessibleName(/Change Zoom in binding: Mouse 5/);
  expect(await persisted(page, 'zoomInButton')).toBe('2');
  await expect(boxes(page, '__zoomIn')).toHaveText(['Mouse 5', 'PageUp']);
});

test('Hotkeys: recording a key over a binding replaces it', async ({ page }) => {
  await page.goto('/');
  await key(page, '__cursorLock').locator('.kchg').click();
  await page.keyboard.press('F9');
  expect(await persisted(page, 'cursorLockVk')).toBe('120');
  await expect(boxes(page, '__cursorLock')).toHaveText(['F9']);
  await expect(page.locator('.capsule')).toHaveCount(0);   // keybinds persist at once, never unsaved
});

test('Hotkeys: hover shows an x that removes just that binding', async ({ page }) => {
  await page.goto('/');
  const kb = key(page, '__zoomIn').locator('.kb').first();   // Mouse 5
  const x = kb.locator('.kx');
  expect(await css(x, 'opacity')).toBe('0');
  await kb.hover();
  await expect.poll(() => css(x, 'opacity')).toBe('1');
  await x.click();
  expect(await persisted(page, 'zoomInButton')).toBe('0');
  expect(await persisted(page, 'zoomInButtonMods')).toBe('0');
  await expect(boxes(page, '__zoomIn')).toHaveText(['PageUp']);   // the key part stays
  await expect(key(page, '__zoomIn').locator('.kc.ghost')).toHaveText('Add key');
  expect(await persisted(page, 'zoomInVk')).toBeUndefined();
});

test('Hotkeys: right-click removes a binding too', async ({ page }) => {
  await page.goto('/');
  await key(page, '__cursorLock').locator('.kchg').click({ button: 'right' });
  expect(await persisted(page, 'cursorLockVk')).toBe('0');
  await expect(key(page, '__cursorLock').locator('.kc.ghost')).toHaveText('Set key');
});

test('Hotkeys: Zoom out takes a second binding in the free slot, the first stays', async ({ page }) => {
  await page.goto('/');
  await key(page, '__zoomOut').locator('.kc.ghost').click();
  await page.keyboard.press('Alt+F5');
  expect(await persisted(page, 'zoomOutVk2')).toBe('116');
  expect(await persisted(page, 'zoomOutMods2')).toBe('2');
  await expect(boxes(page, '__zoomOut')).toHaveText(['F3', 'Alt+F5']);
  await expect(key(page, '__zoomOut').locator('.kc.ghost')).toHaveCount(0);
});

test('Hotkeys: a mouse button with modifiers records, side buttons alone are fine', async ({ page }) => {
  await page.goto('/');
  const ghost = key(page, '__zoomOut').locator('.kc.ghost');
  await ghost.click();
  await page.keyboard.down('Alt');
  const box = await ghost.boundingBox();
  await page.mouse.click(box.x + 10, box.y + 10, { button: 'middle' });
  await page.keyboard.up('Alt');
  expect(await persisted(page, 'zoomOutButton2')).toBe('5');
  expect(await persisted(page, 'zoomOutButton2Mods')).toBe('2');
  await expect(boxes(page, '__zoomOut')).toHaveText(['F3', 'Alt+Middle click']);
});

test('Hotkeys: a wheel notch with a modifier records Wheel up or Wheel down on a zoom row', async ({ page }) => {
  await page.goto('/');
  const ghost = key(page, '__zoomOut').locator('.kc.ghost');
  await ghost.click();
  await page.mouse.move(600, 400);
  await page.keyboard.down('Control');
  await page.mouse.wheel(0, 120);
  await page.keyboard.up('Control');
  expect(await persisted(page, 'zoomOutButton2')).toBe('7');       // wheel down
  expect(await persisted(page, 'zoomOutButton2Mods')).toBe('1');   // Ctrl
  await expect(boxes(page, '__zoomOut')).toHaveText(['F3', 'Ctrl+Wheel down']);
  // And upward, on Zoom in: replace the key part by re-recording PageUp with the wheel.
  await key(page, '__zoomIn').locator('.kchg').nth(1).click();
  await page.mouse.move(600, 400);
  await page.keyboard.down('Alt');
  await page.mouse.wheel(0, -120);
  await page.keyboard.up('Alt');
  expect(await persisted(page, 'zoomInButton2')).toBe('6');       // wheel up
  expect(await persisted(page, 'zoomInButton2Mods')).toBe('2');   // Alt
  await expect(boxes(page, '__zoomIn')).toHaveText(['Mouse 5', 'Alt+Wheel up']);
  await expect(page.locator('.capsule')).toHaveCount(0);
});

test('Hotkeys: a wheel notch without a modifier is refused and the row keeps listening', async ({ page }) => {
  await page.goto('/');
  await key(page, '__zoomOut').locator('.kc.ghost').click();
  await page.mouse.move(600, 400);
  await page.mouse.wheel(0, 120);
  await expect(key(page, '__zoomOut').locator('.refusal')).toContainText('needs a modifier');
  await expect(key(page, '__zoomOut').locator('.kc.live')).toBeVisible();
  expect(await persisted(page, 'zoomOutButton2')).toBeUndefined();
});

test('Hotkeys: the wheel is a zoom binding only; other rows ignore it', async ({ page }) => {
  await page.goto('/');
  await key(page, '__hideCursor').locator('.kc.ghost').click();
  await page.mouse.move(600, 400);
  await page.keyboard.down('Control');
  await page.mouse.wheel(0, 120);
  await page.keyboard.up('Control');
  await expect(key(page, '__hideCursor').locator('.kc.live')).toBeVisible();   // still listening
  expect(await persisted(page, 'hideCursorVk')).toBeUndefined();
  await page.keyboard.press('Escape');
});

test('Hotkeys: a binding takes at most two modifiers', async ({ page }) => {
  await page.goto('/');
  await key(page, '__zoomOut').locator('.kc.ghost').click();
  await page.keyboard.press('Control+Alt+Shift+F5');
  await expect(key(page, '__zoomOut').locator('.refusal')).toContainText('more than two modifiers');
  await expect(key(page, '__zoomOut').locator('.kc.live')).toBeVisible();
  expect(await persisted(page, 'zoomOutVk2')).toBeUndefined();
  await page.keyboard.press('Control+Alt+F5');
  expect(await persisted(page, 'zoomOutVk2')).toBe('116');
  expect(await persisted(page, 'zoomOutMods2')).toBe('3');
});

test('keybind safety: typing keys and system combos are refused and the row keeps listening', async ({ page }) => {
  await page.goto('/');
  const ghost = key(page, '__zoomOut').locator('.kc.ghost');
  await ghost.click();
  await page.keyboard.press('a');
  await expect(key(page, '__zoomOut').locator('.kc.live')).toBeVisible();
  await expect(key(page, '__zoomOut').locator('.refusal')).toContainText('Add Ctrl, Alt or Win');
  expect(await persisted(page, 'zoomOutVk2')).toBeUndefined();
  await page.keyboard.press('F2');
  await expect(key(page, '__zoomOut').locator('.kc.live')).toHaveCount(0);
  expect(await persisted(page, 'zoomOutVk2')).toBe('113');
});

test('a modifier alone never binds; the row keeps listening', async ({ page }) => {
  await page.goto('/');
  await key(page, '__zoomOut').locator('.kc.ghost').click();
  await page.keyboard.press('Control');
  await page.keyboard.press('Shift');
  await expect(key(page, '__zoomOut').locator('.kc.live')).toBeVisible();
  expect(await persisted(page, 'zoomOutVk2')).toBeUndefined();
});

test('Hotkeys: Hide pointer takes one key and no mouse button', async ({ page }) => {
  await page.goto('/');
  const ghost = key(page, '__hideCursor').locator('.kc.ghost');
  await ghost.click();
  const b = await ghost.boundingBox();
  await page.mouse.click(b.x + 10, b.y + 10, { button: 'middle' });   // mouse buttons are for the zoom rows
  await expect(key(page, '__hideCursor').locator('.kc.live')).toBeVisible();
  await page.keyboard.press('F8');
  expect(await persisted(page, 'hideCursorVk')).toBe('119');
  await expect(boxes(page, '__hideCursor')).toHaveText(['F8']);
  await expect(key(page, '__hideCursor').locator('.kc.ghost')).toHaveCount(0);   // one binding only
});

// ---- Hotkeys: pan ---------------------------------------------------------------------------------

const PAN_KEYS = ['panLeftVk', 'panUpVk', 'panRightVk', 'panDownVk', 'panLeftMods', 'panUpMods', 'panRightMods', 'panDownMods'];
const panWrites = async (page) => {
  const w = {};
  for (const m of await sent(page, 'setConfigPersist')) w[m.key] = m.value;
  return w;
};
const PAN_BOUND = { panLeftVk: '37', panUpVk: '38', panRightVk: '39', panDownVk: '40',
  panLeftMods: '3', panUpMods: '3', panRightMods: '3', panDownMods: '3' };

test('Pan: unset it reads [Set modifiers + Arrow keys]; Ctrl+Alt makes one box [Ctrl + Alt + Arrow keys]', async ({ page }) => {
  await page.goto('/');
  const row = key(page, '__pan');
  await expect(row).toHaveCount(1);
  await expect(key(page, '__panLeft')).toHaveCount(0);
  const ghost = row.locator('.kc.ghost');
  await expect(ghost).toHaveText('Set modifiers+Arrow keys');
  await expect(row.locator('.kb')).toHaveCount(0);
  await ghost.click();
  await expect(row.locator('.kc.live')).toContainText('Press modifiers...');
  await page.keyboard.down('Control'); await page.keyboard.down('Alt');
  await page.keyboard.up('Alt'); await page.keyboard.up('Control');
  const w = await panWrites(page);
  expect([w.panLeftVk, w.panUpVk, w.panRightVk, w.panDownVk]).toEqual(['37', '38', '39', '40']);
  for (const k of ['panLeftMods', 'panUpMods', 'panRightMods', 'panDownMods']) expect(w[k]).toBe('3');
  await expect(row.locator('.kb .kc')).toHaveCount(1);
  await expect(row.locator('.kb .kc')).toHaveText('Ctrl+Alt+Arrow keys');
  await expect(row.locator('.kfix')).toHaveText('Arrow keys');   // the fixed part
});

test('Pan: pressing Ctrl+Alt+Left saves only the modifiers', async ({ page }) => {
  await page.goto('/');
  const row = key(page, '__pan');
  await row.locator('.kc.ghost').click();
  await page.keyboard.press('Control+Alt+ArrowLeft');
  const w = await panWrites(page);
  expect([w.panLeftVk, w.panUpVk, w.panRightVk, w.panDownVk]).toEqual(['37', '38', '39', '40']);
  expect(w.panLeftMods).toBe('3');
  await expect(row.locator('.kb .kc')).toHaveText('Ctrl+Alt+Arrow keys');
});

test('Pan: a non-modifier key is ignored; a refused combo and a third modifier say why', async ({ page }) => {
  await page.goto('/');
  const row = key(page, '__pan');
  await row.locator('.kc.ghost').click();
  await page.keyboard.press('a');
  await expect(row.locator('.kc.live')).toBeVisible();
  await page.keyboard.press('Meta+ArrowLeft');
  await expect(row.locator('.refusal')).toContainText('Windows shortcut');
  await page.keyboard.press('Control+Alt+Shift+ArrowLeft');
  await expect(row.locator('.refusal')).toContainText('more than two modifiers');
  await expect(row.locator('.kc.live')).toBeVisible();
  expect((await sent(page, 'setConfigPersist')).filter((m) => m.value !== '0')).toHaveLength(0);
});

test('Pan: re-recording clears the old modifiers at once and Escape restores them', async ({ page }) => {
  await page.addInitScript((p) => { window.__cfgExtra = p; }, PAN_BOUND);
  await page.goto('/');
  const row = key(page, '__pan');
  await expect(row.locator('.kb .kc')).toHaveText('Ctrl+Alt+Arrow keys');
  await row.locator('.kchg').click();
  expect(await persisted(page, 'panLeftVk')).toBe('0');
  await expect(row.locator('.kc.live')).toBeVisible();
  await page.keyboard.press('Escape');
  expect(await persisted(page, 'panLeftVk')).toBe('37');
  expect(await persisted(page, 'panLeftMods')).toBe('3');
  await expect(row.locator('.kb .kc')).toHaveText('Ctrl+Alt+Arrow keys');
});

test('Pan: a bound row unbinds with right-click or the x and writes all eight keys to 0', async ({ page }) => {
  await page.addInitScript((p) => { window.__cfgExtra = p; }, PAN_BOUND);
  await page.goto('/');
  const row = key(page, '__pan');
  await row.locator('.kchg').click({ button: 'right' });
  const w = await panWrites(page);
  for (const k of PAN_KEYS) expect(w[k], k).toBe('0');
  await expect(row.locator('.kc.ghost')).toHaveText('Set modifiers+Arrow keys');
});

test('Pan: older custom pan keys read as Custom keys; choosing modifiers replaces all four; the x resets', async ({ page }) => {
  await page.addInitScript(() => { window.__cfgExtra = { panLeftVk: '33', panUpVk: '38', panRightVk: '39', panDownVk: '40',
    panLeftMods: '3', panUpMods: '3', panRightMods: '3', panDownMods: '3' }; });
  await page.goto('/');
  const row = key(page, '__pan');
  await expect(row.locator('.kb .kc')).toHaveText('Custom keys');
  // Arming must not wipe the custom keys.
  await row.locator('.kchg').click();
  expect((await sent(page, 'setConfigPersist')).filter((m) => PAN_KEYS.includes(m.key))).toHaveLength(0);
  await page.keyboard.press('Control+Alt+ArrowUp');
  const w = await panWrites(page);
  expect([w.panLeftVk, w.panUpVk, w.panRightVk, w.panDownVk]).toEqual(['37', '38', '39', '40']);
  await expect(row.locator('.kb .kc')).toHaveText('Ctrl+Alt+Arrow keys');
});

test('Pan: the x clears custom pan keys', async ({ page }) => {
  await page.addInitScript(() => { window.__cfgExtra = { panLeftVk: '33', panLeftMods: '3' }; });
  await page.goto('/');
  const row = key(page, '__pan');
  await row.locator('.kb').hover();
  await row.getByRole('button', { name: 'Remove pan keys' }).click();
  const w = await panWrites(page);
  for (const k of PAN_KEYS) expect(w[k], k).toBe('0');
  await expect(row.locator('.kc.ghost')).toHaveText('Set modifiers+Arrow keys');
});

// ---- Hotkeys: extra-key switches --------------------------------------------------------------------

test('Extra keys: each has an on/off switch, on by default; off dims the binding but keeps it', async ({ page }) => {
  await page.addInitScript(() => { window.__cfgExtra = { hideCursorVk: '119', ...{ panLeftVk: '37', panUpVk: '38', panRightVk: '39', panDownVk: '40',
    panLeftMods: '3', panUpMods: '3', panRightMods: '3', panDownMods: '3' } }; });
  await page.goto('/');
  for (const [k, name] of [['__pan', 'Pan with the arrow keys'], ['__hideCursor', 'Hide pointer'], ['__cursorLock', 'Inspect mode']]) {
    await expect(key(page, k).getByRole('switch', { name })).toBeChecked();
    await expect(key(page, k).locator('.kwrap')).not.toHaveClass(/off/);
  }
  // The Zoom rows have no switch.
  await expect(key(page, '__zoomIn').getByRole('switch')).toHaveCount(0);
  const sw = key(page, '__hideCursor').getByRole('switch');
  await sw.uncheck({ force: true });
  await expect(key(page, '__hideCursor').locator('.kwrap')).toHaveClass(/off/);
  await expect.poll(() => css(key(page, '__hideCursor').locator('.kwrap'), 'opacity')).toBe('0.35');   // after its short fade
  await expect(boxes(page, '__hideCursor')).toHaveText(['F8']);   // still shown, still stored
  expect((await sent(page, 'setConfig')).filter((m) => m.key === 'hideCursorOn').map((m) => String(m.value))).toEqual(['0']);
  expect(await persisted(page, 'hideCursorVk')).toBeUndefined();
  // A switch is an ordinary session change (the capsule counts it), unlike the binding itself.
  await expect(page.locator('.capsule')).toContainText('1 unsaved change');
  await sw.check({ force: true });
  await expect(key(page, '__hideCursor').locator('.kwrap')).not.toHaveClass(/off/);
  await expect(page.locator('.capsule')).toHaveCount(0);
});

test('Extra keys: pan and Inspect switches write their own keys', async ({ page }) => {
  await page.goto('/');
  await key(page, '__pan').getByRole('switch').uncheck({ force: true });
  await key(page, '__cursorLock').getByRole('switch').uncheck({ force: true });
  const w = Object.fromEntries((await sent(page, 'setConfig')).map((m) => [m.key, String(m.value)]));
  expect(w.panKeysOn).toBe('0');
  expect(w.cursorLockOn).toBe('0');
  await expect(page.locator('.capsule')).toContainText('2 unsaved changes');
});

test('Extra keys: a binding can still be edited while its switch is off', async ({ page }) => {
  await page.addInitScript(() => { window.__cfgExtra = { hideCursorOn: '0' }; });
  await page.goto('/');
  await expect(key(page, '__hideCursor').locator('.kwrap')).toHaveClass(/off/);
  await expect(key(page, '__hideCursor').getByRole('switch')).not.toBeChecked();
  await key(page, '__hideCursor').locator('.kc.ghost').click();
  await page.keyboard.press('F8');
  expect(await persisted(page, 'hideCursorVk')).toBe('119');
});

// ---- Hotkeys: the old rules ------------------------------------------------------------------------

test('Escape cancels an armed capture and keeps the old binding', async ({ page }) => {
  await page.goto('/');
  const cap = key(page, '__cursorLock').locator('.kchg');
  await expect(cap).toHaveAccessibleName(/F2/);
  await cap.click();
  await expect(cap).toHaveAccessibleName(/Recording/);
  await page.keyboard.press('Escape');
  await expect(cap).toHaveAccessibleName(/Change Inspect mode binding: F2/);
  await expect(boxes(page, '__cursorLock')).toHaveText(['F2']);
});

test('stored binds the rules refuse are reset once and persisted', async ({ page }) => {
  await page.addInitScript(() => { window.__cfgExtra = { zoomInVk: '65' }; });
  await page.goto('/');
  await expect(page.locator('h1')).toHaveText('Hotkeys');
  await expect.poll(async () => (await sent(page, 'setConfigPersist')).some((m) => m.key === 'zoomInVk' && m.value === '0')).toBe(true);
  await expect(page.locator('.capsule')).toHaveCount(0);
});

test('no reset when every stored bind is allowed', async ({ page }) => {
  await page.goto('/');
  await expect(page.locator('h1')).toHaveText('Hotkeys');
  expect(await sent(page, 'setConfigPersist')).toHaveLength(0);
});

test('wheel binds stored by the migration are allowed and shown', async ({ page }) => {
  await page.addInitScript(() => { window.__cfgExtra = { zoomInButton: '0', zoomInVk: '0', zoomInButton2: '6', zoomInButton2Mods: '1',
    zoomOutButton: '7', zoomOutButtonMods: '1', zoomOutVk: '0' }; });
  await page.goto('/');
  await expect(boxes(page, '__zoomIn')).toHaveText(['Ctrl+Wheel up']);
  await expect(boxes(page, '__zoomOut')).toHaveText(['Ctrl+Wheel down']);
  expect(await sent(page, 'setConfigPersist')).toHaveLength(0);
});

test('a settings write the host could not save shows a dialog', async ({ page }) => {
  await page.goto('/');
  await expect(page.locator('h1')).toHaveText('Hotkeys');
  await page.evaluate(() => window.__hostSend({ type: 'configWriteFailed', key: 'maxLevel' }));
  await expect(page.getByRole('dialog')).toContainText('maxLevel');
});
