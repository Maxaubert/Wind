// Settings "Tray menu" tab (#313): the Performance switch, the Sliders and Toggles lists with
// check, drag and keyboard reorder, the four-slider cap, and the five global ini keys written
// through the mock bridge. Plus the pure list model that mirrors src/tray_items.cpp.
import { test, expect } from '@playwright/test';
import { parseTray, parseList, serialiseList, MAX_SLIDERS, SLIDERS, TOGGLES } from '../src/tray/trayModel.js';

test.beforeEach(async ({ page }) => {
  await page.addInitScript(() => {
    window.__skipSplash = true;
    window.__msgs = [];
    const init = () => {
      if (window.__live) return;
      window.__live = { maxLevel: '12', model: 'hybrid', uiTheme: 'dark', ...(window.__cfgExtra || {}) };
      window.__saved = { ...window.__live };
    };
    const listeners = new Set();
    const send = (data) => listeners.forEach((fn) => fn({ data }));
    window.chrome = { webview: {
      addEventListener: (_e, fn) => listeners.add(fn),
      postMessage: (msg) => {
        window.__msgs.push(msg);
        if (msg.type === 'getConfig') { init(); send({ type: 'config', values: { ...window.__live }, saved: { ...window.__saved },
          profiles: { names: ['Default'], active: 'Default' } }); }
        else if (msg.type === 'setConfig') window.__live[msg.key] = msg.value;
        else if (msg.type === 'mpoState') send({ type: 'mpoState', disabled: false, bootKnown: true, atBoot: false });
      },
    } };
  });
});

const open = async (page) => { await page.goto('/'); await page.locator('.side .it[data-g="tray"]').click(); };
const live = (page) => page.evaluate(() => ({ ...window.__live }));
const rowsOf = (page, list) => page.locator('#cap-' + list).locator('xpath=ancestor::div[contains(@class,"tcap")]/following-sibling::div[contains(@class,"tlist")][1]').locator('.trow');
const names = (page, list) => rowsOf(page, list).locator('.k').allTextContents();
const chk = (page, list, name) => rowsOf(page, list).filter({ hasText: name }).locator('.chk');

// ---- the pure model ----------------------------------------------------------------------------

test('model: defaults are Warmth and Brightness on, everything else off, performance off', () => {
  const t = parseTray({});
  expect(t.perf).toBe(false);
  expect(t.sliders.map((i) => i.key)).toEqual(SLIDERS.map((i) => i.key));
  expect(t.sliders.filter((i) => i.on).map((i) => i.key)).toEqual(['colorWarmPct', 'colorDimPct']);
  expect(t.toggles.map((i) => i.key)).toEqual(['trackCaret', 'trackFocus', 'keepEdges', 'engine']);
  expect(t.toggles.some((i) => i.on)).toBe(false);
});

test('model: round trip keeps order and enabled, drops unknown keys, appends missing items off', () => {
  const v = { traySliders: 'zoomInSpeed,colorWarmPct,bogus', traySliderOrder: 'panSpeed,zoomInSpeed,nope,colorWarmPct',
              trayToggles: 'keepEdges', trayToggleOrder: 'keepEdges,trackCaret' };
  const s = parseList(v, 'sliders');
  expect(s.map((i) => i.key)).toEqual(['panSpeed', 'zoomInSpeed', 'colorWarmPct', 'colorDimPct', 'maxLevel', 'zoomOutSpeed', 'cursorSmoothing', 'zoomEaseOutMs']);
  expect(s.filter((i) => i.on).map((i) => i.key)).toEqual(['zoomInSpeed', 'colorWarmPct']);
  const w = serialiseList('sliders', s);
  expect(w.traySliders).toBe('zoomInSpeed,colorWarmPct');
  expect(w.traySliderOrder).toBe(s.map((i) => i.key).join(','));
  const back = parseList({ ...v, ...w }, 'sliders');
  expect(back.map((i) => [i.key, i.on])).toEqual(s.map((i) => [i.key, i.on]));
  const t = parseList(v, 'toggles');
  expect(t.map((i) => i.key)).toEqual(['keepEdges', 'trackCaret', 'trackFocus', 'engine']);
  expect(t.filter((i) => i.on).map((i) => i.key)).toEqual(['keepEdges']);
});

test('model: more than four enabled sliders read the first four as on, toggles are uncapped', () => {
  const all = SLIDERS.map((i) => i.key).join(',');
  const s = parseList({ traySliders: all }, 'sliders');
  expect(s.filter((i) => i.on).map((i) => i.key)).toEqual(SLIDERS.slice(0, MAX_SLIDERS).map((i) => i.key));
  const t = parseList({ trayToggles: TOGGLES.map((i) => i.key).join(',') }, 'toggles');
  expect(t.every((i) => i.on)).toBe(true);
  expect(parseList({ traySliders: '' }, 'sliders').some((i) => i.on)).toBe(false);   // an empty list is all off, not the defaults
});

// ---- the tab -------------------------------------------------------------------------------------

test('tab: TRAY section label, banner, performance card and both lists', async ({ page }) => {
  await page.goto('/');
  await expect(page.locator('.side .grp2 .lbl')).toHaveText('Tray');
  await expect(page.locator('.side .grp2 .it')).toHaveText(/Tray menu/);
  await expect(page.locator('.side nav .it[data-g="tray"]')).toHaveCount(0);
  await page.locator('.side .it[data-g="tray"]').click();
  await expect(page.locator('h1')).toHaveText('Tray menu');
  await expect(page.locator('.bdesc')).toHaveText('Choose what the tray menu shows, and in what order.');
  await expect(page.getByRole('switch', { name: 'Performance in the tray' })).not.toBeChecked();
  await expect(page.locator('#cap-sliders')).toHaveText('Sliders');
  await expect(page.locator('#cap-toggles')).toHaveText('Toggles');
  expect(await names(page, 'sliders')).toEqual(['Warmth', 'Brightness', 'Max zoom', 'Zoom-in speed', 'Zoom-out speed', 'Pan speed', 'Pan smoothing', 'Release glide']);
  expect(await names(page, 'toggles')).toEqual(['Follow the text cursor', 'Follow keyboard focus', 'Keep within the edges', 'Magnifier engine']);
  await expect(page.locator('[data-cnt="sliders"]')).toHaveText('2 of 4');
  await expect(page.locator('[data-cnt="toggles"]')).toHaveText('0 on');
  await expect(chk(page, 'sliders', 'Warmth')).toHaveAttribute('aria-checked', 'true');
  await expect(chk(page, 'sliders', 'Max zoom')).toHaveAttribute('aria-checked', 'false');
});

test('tab: item icons are bare line icons without a background box', async ({ page }) => {
  await open(page);
  const tic = page.locator('.tic').first();
  const st = await tic.evaluate((el) => { const c = getComputedStyle(el); return { bg: c.backgroundColor, bw: c.borderTopWidth, br: c.borderTopLeftRadius }; });
  expect(st.bg).toBe('rgba(0, 0, 0, 0)');
  expect(st.bw).toBe('0px');
  expect(await page.locator('.tic svg').count()).toBe(12);
});

test('performance switch writes trayPerf', async ({ page }) => {
  await open(page);
  await page.getByRole('switch', { name: 'Performance in the tray' }).check({ force: true });
  expect((await live(page)).trayPerf).toBe('1');
  await page.getByRole('switch', { name: 'Performance in the tray' }).uncheck({ force: true });
  expect((await live(page)).trayPerf).toBe('0');
});

test('check and uncheck write the enabled list and the full order', async ({ page }) => {
  await open(page);
  await chk(page, 'sliders', 'Pan speed').click();
  let v = await live(page);
  expect(v.traySliders).toBe('colorWarmPct,colorDimPct,panSpeed');
  expect(v.traySliderOrder).toBe('colorWarmPct,colorDimPct,maxLevel,zoomInSpeed,zoomOutSpeed,panSpeed,cursorSmoothing,zoomEaseOutMs');
  await expect(page.locator('[data-cnt="sliders"]')).toHaveText('3 of 4');
  await chk(page, 'sliders', 'Warmth').click();
  v = await live(page);
  expect(v.traySliders).toBe('colorDimPct,panSpeed');
  // a click anywhere on the row toggles it too
  await rowsOf(page, 'toggles').filter({ hasText: 'Follow keyboard focus' }).locator('.k').click();
  v = await live(page);
  expect(v.trayToggles).toBe('trackFocus');
  expect(v.trayToggleOrder).toBe('trackCaret,trackFocus,keepEdges,engine');
  await expect(page.locator('[data-cnt="toggles"]')).toHaveText('1 on');
});

test('slider cap: at four the unchecked boxes are disabled with a note, unchecking frees them', async ({ page }) => {
  await open(page);
  await chk(page, 'sliders', 'Max zoom').click();
  await chk(page, 'sliders', 'Pan speed').click();
  await expect(page.locator('[data-cnt="sliders"]')).toHaveText('4 of 4 · Uncheck one to add another');
  await expect(chk(page, 'sliders', 'Zoom-in speed')).toBeDisabled();
  await expect(chk(page, 'sliders', 'Release glide')).toBeDisabled();
  await expect(chk(page, 'sliders', 'Warmth')).toBeEnabled();
  const before = (await live(page)).traySliders;
  await rowsOf(page, 'sliders').filter({ hasText: 'Zoom-in speed' }).locator('.k').click();   // row click is refused too
  expect((await live(page)).traySliders).toBe(before);
  // toggles are not capped
  for (const n of ['Follow the text cursor', 'Follow keyboard focus', 'Keep within the edges']) await chk(page, 'toggles', n).click();
  await expect(page.locator('[data-cnt="toggles"]')).toHaveText('3 on');
  await chk(page, 'sliders', 'Warmth').click();
  await expect(page.locator('[data-cnt="sliders"]')).toHaveText('3 of 4');
  await expect(chk(page, 'sliders', 'Zoom-in speed')).toBeEnabled();
});

test('tray changes are not unsaved changes and never show the capsule', async ({ page }) => {
  await open(page);
  await chk(page, 'sliders', 'Pan speed').click();
  await page.getByRole('switch', { name: 'Performance in the tray' }).check({ force: true });
  await expect(page.locator('.capsule')).toHaveCount(0);
});

const dragRow = async (page, list, from, to) => {
  const rows = rowsOf(page, list);
  await rows.first().scrollIntoViewIfNeeded();
  await rows.last().scrollIntoViewIfNeeded();
  const a = await rows.filter({ hasText: from }).locator('.tic').boundingBox();
  const b = await rows.filter({ hasText: to }).locator('.tic').boundingBox();
  await page.mouse.move(a.x + a.width / 2, a.y + a.height / 2);
  await page.mouse.down();
  await page.mouse.move(a.x + a.width / 2, a.y + a.height / 2 + (b.y > a.y ? 8 : -8), { steps: 3 });
  await page.mouse.move(b.x + b.width / 2, b.y + b.height / 2 + (b.y > a.y ? 6 : -6), { steps: 12 });
  await page.mouse.up();
};

test('drag reorders the Sliders list and writes the order, enabled flags kept', async ({ page }) => {
  await open(page);
  await dragRow(page, 'sliders', 'Pan speed', 'Warmth');
  await expect.poll(async () => (await names(page, 'sliders'))[0]).toBe('Pan speed');
  const v = await live(page);
  expect(v.traySliderOrder.split(',').slice(0, 3)).toEqual(['panSpeed', 'colorWarmPct', 'colorDimPct']);
  expect(v.traySliders).toBe('colorWarmPct,colorDimPct');
  await expect(chk(page, 'sliders', 'Warmth')).toHaveAttribute('aria-checked', 'true');
  await expect(page.locator('.trow.dragging')).toHaveCount(0);
});

test('drag reorders the Toggles list, and a row dragged in one list stays in it', async ({ page }) => {
  await page.setViewportSize({ width: 1120, height: 1100 });   // the whole Sliders card in view: the drag spans all eight rows
  await open(page);
  await dragRow(page, 'toggles', 'Keep within the edges', 'Follow the text cursor');
  await expect.poll(() => names(page, 'toggles')).toEqual(['Keep within the edges', 'Follow the text cursor', 'Follow keyboard focus', 'Magnifier engine']);
  expect((await live(page)).trayToggleOrder).toBe('keepEdges,trackCaret,trackFocus,engine');
  // a slider row dragged to the bottom of its own list lands last and leaves the Toggles card alone
  await dragRow(page, 'sliders', 'Warmth', 'Release glide');
  await expect.poll(async () => (await names(page, 'sliders')).at(-1)).toBe('Warmth');
  expect(await rowsOf(page, 'toggles').count()).toBe(4);
  expect(await rowsOf(page, 'sliders').count()).toBe(8);
  expect(await names(page, 'toggles')).toEqual(['Keep within the edges', 'Follow the text cursor', 'Follow keyboard focus', 'Magnifier engine']);
});

test('keyboard: Space picks up, arrows move, Space drops; Escape cancels', async ({ page }) => {
  await open(page);
  const grip = (list, name) => rowsOf(page, list).filter({ hasText: name }).locator('.grip');
  await grip('sliders', 'Warmth').focus();
  await page.keyboard.press('Space');
  await expect(grip('sliders', 'Warmth')).toHaveAttribute('aria-pressed', 'true');
  await page.keyboard.press('ArrowDown');
  await page.keyboard.press('ArrowDown');
  await expect.poll(async () => (await names(page, 'sliders')).slice(0, 3)).toEqual(['Brightness', 'Max zoom', 'Warmth']);
  await expect(grip('sliders', 'Warmth')).toBeFocused();
  await page.keyboard.press('Space');
  await expect(grip('sliders', 'Warmth')).toHaveAttribute('aria-pressed', 'false');
  expect((await live(page)).traySliderOrder.split(',').slice(0, 3)).toEqual(['colorDimPct', 'maxLevel', 'colorWarmPct']);
  // Escape puts it back
  await page.keyboard.press('Space');
  await page.keyboard.press('ArrowUp');
  await page.keyboard.press('Escape');
  await expect.poll(async () => (await names(page, 'sliders')).slice(0, 3)).toEqual(['Brightness', 'Max zoom', 'Warmth']);
  // toggles list: move the last one to the top
  await grip('toggles', 'Keep within the edges').focus();
  await page.keyboard.press('Space');
  await page.keyboard.press('ArrowUp');
  await page.keyboard.press('ArrowUp');
  await page.keyboard.press('Space');
  await expect.poll(() => names(page, 'toggles')).toEqual(['Keep within the edges', 'Follow the text cursor', 'Follow keyboard focus', 'Magnifier engine']);
  expect((await live(page)).trayToggleOrder).toBe('keepEdges,trackCaret,trackFocus,engine');
});

test('the saved lists load back from the ini', async ({ page }) => {
  await page.addInitScript(() => { window.__cfgExtra = { trayPerf: '1', traySliders: 'zoomOutSpeed', traySliderOrder: 'zoomOutSpeed,colorDimPct',
    trayToggles: 'keepEdges', trayToggleOrder: 'keepEdges' }; });
  await open(page);
  await expect(page.getByRole('switch', { name: 'Performance in the tray' })).toBeChecked();
  expect((await names(page, 'sliders')).slice(0, 2)).toEqual(['Zoom-out speed', 'Brightness']);
  await expect(chk(page, 'sliders', 'Zoom-out speed')).toHaveAttribute('aria-checked', 'true');
  await expect(chk(page, 'sliders', 'Warmth')).toHaveAttribute('aria-checked', 'false');
  await expect(page.locator('[data-cnt="sliders"]')).toHaveText('1 of 4');
  await expect(chk(page, 'toggles', 'Keep within the edges')).toHaveAttribute('aria-checked', 'true');
});

test('search finds the Performance row and jumps to the tray tab', async ({ page }) => {
  await page.goto('/');
  await page.locator('.side .search input').fill('performance');
  await page.locator('.side .search input').press('Enter');
  await expect(page.locator('h1')).toHaveText('Tray menu');
});

test('engine: the Magnifier engine item is off by default, has an icon and a description, and writes trayToggles', async ({ page }) => {
  await open(page);
  const row = rowsOf(page, 'toggles').filter({ hasText: 'Magnifier engine' });
  await expect(row.locator('.tic svg')).toHaveCount(1);
  await expect(row.locator('.d')).not.toBeEmpty();
  await expect(chk(page, 'toggles', 'Magnifier engine')).toHaveAttribute('aria-checked', 'false');
  await chk(page, 'toggles', 'Magnifier engine').click();
  const v = await live(page);
  expect(v.trayToggles).toBe('engine');
  expect(v.trayToggleOrder).toBe('trackCaret,trackFocus,keepEdges,engine');
  await expect(page.locator('[data-cnt="toggles"]')).toHaveText('1 on');
});

test('removed tools: Mouse lock, Pass keys and Pause Wind are gone, and old ini keys for them are dropped', async ({ page }) => {
  await page.addInitScript(() => { window.__cfgExtra = {
    trayToggles: 'trackCaret,fixLock,pause,fixPass', trayToggleOrder: 'pause,fixPass,fixLock,trackCaret,keepEdges' }; });
  await open(page);
  const n = await names(page, 'toggles');
  expect(n).toEqual(['Follow the text cursor', 'Keep within the edges', 'Follow keyboard focus', 'Magnifier engine']);
  for (const gone of ['Mouse lock', 'Pass keys', 'Pause Wind']) expect(n.some((x) => x.includes(gone))).toBe(false);
  await expect(page.locator('[data-cnt="toggles"]')).toHaveText('1 on');
});
