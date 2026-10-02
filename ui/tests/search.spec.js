// Settings search (#317): the pure ranker/fuzzy matcher, then the results page against the mock bridge
// (ranked list, the real controls edited in place, advanced rows, tray lists never indexed).
import { test, expect } from '@playwright/test';
import { readFileSync } from 'node:fs';
import { groups, allRows } from '../src/settings-schema.js';
import { search, editDistance, words } from '../src/search/search.js';

test.beforeEach(async ({ page }) => {
  await page.addInitScript(() => {
    window.__skipSplash = true;
    window.__msgs = [];
    const listeners = new Set();
    const send = (data) => listeners.forEach((fn) => fn({ data }));
    window.__live = { maxLevel: '12', model: 'hybrid', uiTheme: 'dark' };
    window.chrome = { webview: {
      addEventListener: (_e, fn) => listeners.add(fn),
      postMessage: (msg) => {
        window.__msgs.push(msg);
        if (msg.type === 'getConfig') send({ type: 'config', values: { ...window.__live }, saved: { ...window.__live },
          profiles: { names: ['Default'], active: 'Default' } });
        else if (msg.type === 'setConfig') window.__live[msg.key] = msg.value;
        else if (msg.type === 'mpoState') send({ type: 'mpoState', disabled: false, bootKnown: true, atBoot: false });
      },
    } };
  });
});

const input = (page) => page.getByLabel('Search settings');
const keys = (q) => search(groups, q).map((h) => h.key);
const rank = (q, key) => keys(q).indexOf(key);

// ---- the pure matcher -------------------------------------------------------------------------

test('words() ignores case, accents and punctuation; editDistance counts swaps as one', () => {
  expect(words('Zoom-in  SPEED!')).toEqual(['zoom', 'in', 'speed']);
  expect(words('Café')).toEqual(['cafe']);
  expect(editDistance('centre', 'center')).toBe(1);
  expect(editDistance('abc', 'abc')).toBe(0);
  expect(editDistance('warmth', 'brightness', 2)).toBeGreaterThan(2);
});

test('empty and unmatched queries return nothing', () => {
  expect(search(groups, '')).toEqual([]);
  expect(search(groups, '  ,, ')).toEqual([]);
  expect(keys('xyzzy')).toEqual([]);
  expect(keys('zoom xyzzy')).toEqual([]);   // every word must match
});

test('stopwords never outrank content words and fuzzy skips free text', () => {
  const zi = search(groups, 'zoom in').map((h) => h.label);
  expect(zi[0]).toBe('Zoom in');
  expect(zi.indexOf('Zoom out')).toBeGreaterThan(-1);
  expect(zi.indexOf('Zoom out')).toBeLessThan(zi.indexOf('Performance in the tray'));
  expect(search(groups, 'tray').map((h) => h.label)).toEqual(['Performance in the tray']);
  expect(search(groups, 'theme').map((h) => h.label)).toEqual(['Theme']);
});

test('results are one flat list ranked best first, with the tab name on each hit', () => {
  const hits = search(groups, 'zoom');
  expect(hits.length).toBeGreaterThan(5);
  for (let i = 1; i < hits.length; i++) expect(hits[i - 1].score).toBeGreaterThanOrEqual(hits[i].score);
  expect(hits[0].groupLabel).toBeTruthy();
  expect(hits[0].row.key).toBe(hits[0].key);
  // A label hit beats a description-only or tab-name-only hit.
  expect(rank('zoom', 'maxLevel')).toBeLessThan(rank('zoom', 'trackCaret') === -1 ? 999 : rank('zoom', 'trackCaret'));
});

test('ranking tiers: exact label > label prefix > label substring > keyword > description > caption', () => {
  // Exact whole label first.
  expect(keys('engine')[0]).toBe('model');
  expect(keys('warmth')[0]).toBe('colorWarmPct');
  expect(keys('zoom in')[0]).toBe('__zoomIn');
  // Label word-prefix beats a keyword-only hit ("glide" is a label word, "coast" only a keyword).
  expect(keys('glide')).toContain('zoomEaseOutMs');
  const labelPrefix = search(groups, 'brigh')[0];
  expect(labelPrefix.key).toBe('colorDimPct');
  // Label hit beats keyword hit: "speed" is in several labels, "velocity" only a keyword of them.
  expect(rank('speed', 'zoomInSpeed')).toBeLessThan(rank('speed', 'zoomEaseOutMs') === -1 ? 999 : rank('speed', 'zoomEaseOutMs'));
  // Keyword beats description: "caret" is a keyword of the text cursor rows.
  const caret = keys('caret');
  expect(caret.slice(0, 2).sort()).toEqual(['trackAlign', 'trackCaret']);
  // Description-only hit ranks below a label hit: "pointer" is a label word of Pointer position and
  // High resolution cursor's description.
  expect(rank('pointer', 'mouseAlign')).toBeLessThan(rank('pointer', 'txSamplingMode'));
  // Card caption and tab name still find rows, last.
  expect(keys('troubleshooting')).toEqual(expect.arrayContaining(['diagnostics', '__diagnostics', '__openIni']));
  expect(keys('preferences')).toContain('uiPalette');
});

test('multi-word queries: every word matches something, scores add up', () => {
  expect(keys('mouse speed')[0]).toBe('cursorSensitivity');
  expect(keys('speed mouse')[0]).toBe('cursorSensitivity');
  expect(keys('soft start length')[0]).toBe('smoothZoomRamp');
  expect(keys('zoom wheel')).toEqual(expect.arrayContaining(['__zoomIn', '__zoomOut']));
});

test('typo tolerance: distance 1 for 4-7 letters, 2 for 8+, prefixes, substrings, phonetic spellings', () => {
  expect(keys('warmht')).toContain('colorWarmPct');          // transposition, 6 letters
  expect(keys('brigthness')[0]).toBe('colorDimPct');         // 10 letters
  expect(keys('brihtness')[0]).toBe('colorDimPct');          // dropped letter
  expect(keys('inertai')).toEqual(expect.arrayContaining(['zoomEaseOutMs']));
  expect(keys('smothing')).toContain('cursorSmoothing');
  expect(keys('sensitivty')).toContain('cursorSensitivity'); // keyword, 10 letters, one missing
  expect(keys('nite light')).toEqual(expect.arrayContaining(['colorWarmPct']));
  expect(keys('nite light')[0]).toBe('colorWarmPct');
  expect(keys('mouze')).toContain('cursorSensitivity');
  expect(keys('MAX ZO')).toContain('maxLevel');              // case-insensitive prefix
  expect(keys('zoomin')).toContain('__zoomIn');              // joined form of "Zoom-in"
  expect(keys('xom')).toEqual([]);                           // 3 letters: no typo budget
});

test('meta keywords reach rows without their label words, incl. the centred-cursor example', () => {
  for (const q of ['mouse position', 'center', 'centre', 'centered', 'centred', 'edge', 'edges']) {
    expect(keys(q)).toEqual(expect.arrayContaining(['mouseAlign', 'trackAlign']));
  }
  expect(keys('mouse position').slice(0, 2).sort()).toEqual(['mouseAlign', 'trackAlign']);
  expect(keys('magnify')).toEqual(expect.arrayContaining(['__zoomIn']));
  expect(keys('hotkey')).toEqual(expect.arrayContaining(['__zoomIn', '__zoomOut']));
  expect(keys('night light')[0]).toBe('colorWarmPct');
  expect(keys('blue light')[0]).toBe('colorWarmPct');
  expect(keys('dim')).toContain('colorDimPct');
  expect(keys('renderer')).toEqual(expect.arrayContaining(['model', 'engineGame']));
  expect(keys('freeze')).toContain('__cursorLock');
  expect(keys('crosshair')).toContain('__cursorLock');
  expect(keys('arrows')).toContain('__pan');
  expect(keys('acceleration')).toContain('smoothZoomAccel');
  expect(keys('momentum')).toEqual(expect.arrayContaining(['zoomEaseOutMs']));
  expect(keys('preset')).toContain('__profiles');
});

test('every searchable row has meta keywords, and no keyword is empty or has an em-dash', () => {
  const rows = allRows.filter((r) => r.label);
  expect(rows.length).toBeGreaterThan(30);
  for (const r of rows) {
    expect(Array.isArray(r.keywords) && r.keywords.length >= 3, r.key + ' needs keywords').toBe(true);
    for (const k of r.keywords) expect(words(k).length, r.key + ' keyword ' + k).toBeGreaterThan(0);
  }
});

test('advanced rows are searchable and say so; removed and hero rows are not indexed', () => {
  const engine = search(groups, 'engine');
  expect(engine.find((h) => h.key === 'model').adv).toBe(true);
  expect(engine.find((h) => h.key === 'engineGame').adv).toBe(true);
  expect(search(groups, 'maximum').every((h) => !h.adv)).toBe(true);
  expect(keys('advanced')).toEqual(expect.arrayContaining(['model', 'noSwallowApps', 'lockApps', 'smoothZoomAccel']));
  expect(keys('advanced')).not.toContain('maxLevel');
  expect(keys('render')).not.toContain('renderExclude');
  expect(keys('logo')).not.toContain('__about');
});

test('Tray menu: only its normal switch row is indexed, never the item lists', () => {
  expect(keys('performance')).toContain('trayPerf');
  const trayRows = groups.find((g) => g.id === 'tray').cards.flatMap((c) => c.rows);
  expect(trayRows.map((r) => r.key)).toEqual(['trayPerf']);
  // The lists are drawn by the custom page, so no key of theirs is a schema row.
  for (const k of ['trayTools', 'traySliders', 'trayToggles', 'trayOrder', 'keepEdges']) expect(allRows.map((r) => r.key)).not.toContain(k);
  for (const q of ['slider', 'toggle', 'quick', 'segment', 'drag', 'reorder', 'keep cursor centred'])
    expect(keys(q).filter((k) => k.startsWith('tray') && k !== 'trayPerf')).toEqual([]);
});

test('search files contain no em-dash', () => {
  for (const f of ['../src/search/search.js', '../src/search/Results.svelte', '../src/settings-schema.js'])
    expect(readFileSync(new URL(f, import.meta.url), 'utf8')).not.toContain(String.fromCharCode(0x2014));
});

// ---- the results page --------------------------------------------------------------------------

test('typing shows one ranked list of real rows with a quiet tab name each', async ({ page }) => {
  await page.goto('/');
  await expect(page.locator('h1')).toHaveText('Hotkeys');
  await input(page).fill('zoom');
  await expect(page.locator('.res')).toBeVisible();
  await expect(page.locator('.res .gl')).toHaveCount(0);   // no per-tab grouping
  const shown = await page.locator('.res .hit').evaluateAll((els) => els.map((e) => e.getAttribute('data-hit')));
  expect(shown).toEqual(keys('zoom'));                       // the page order IS the rank
  await expect(page.locator('.res .hit[data-hit="maxLevel"] .tab')).toHaveText('Zoom');
  await expect(page.locator('.res .hit[data-hit="__zoomIn"] .tab')).toHaveText('Hotkeys');
  // Real controls: a slider, a keybind box.
  await expect(page.locator('.res .hit[data-hit="maxLevel"] input[type=range]')).toBeVisible();
  await expect(page.locator('.res .hit[data-hit="__zoomIn"] .bx')).toBeVisible();
});

test('a result control changes the setting in place, no navigation, the live ini follows', async ({ page }) => {
  await page.goto('/');
  await input(page).fill('keyboard focus');
  const sw = page.locator('.res .hit[data-hit="trackFocus"]').getByRole('switch');
  await expect(sw).not.toBeChecked();
  await sw.click({ force: true });
  await expect(sw).toBeChecked();
  expect(await page.evaluate(() => window.__live.trackFocus)).toBe('1');
  await expect(page.locator('.res')).toBeVisible();          // still on the results
  await expect(page.locator('.side .it.sel')).toHaveCount(0);
  await expect(page.locator('.capsule')).toBeVisible();      // the session shows it as unsaved, like on its page
  // The page shows the same value.
  await input(page).press('Escape');
  await page.locator('.side .it[data-g="view"]').click();
  await expect(page.locator('[data-key="trackFocus"]').getByRole('switch')).toBeChecked();
});

test('a select and a slider edit in place too', async ({ page }) => {
  await page.goto('/');
  await input(page).fill('mouse position');
  const row = page.locator('.res .hit[data-hit="mouseAlign"]');
  await expect(row).toBeVisible();
  await row.locator('select').selectOption('1');
  expect(await page.evaluate(() => window.__live.mouseAlign)).toBe('1');
  await input(page).fill('max zoom');
  const sl = page.locator('.res .hit[data-hit="maxLevel"] input[type=range]');
  await sl.focus();
  await page.keyboard.press('ArrowRight');
  expect(await page.evaluate(() => window.__live.maxLevel)).toBe('13');
  await expect(page.locator('.res')).toBeVisible();
});

test('advanced rows match and are editable from search while Show advanced settings is off', async ({ page }) => {
  await page.goto('/');
  await page.locator('.side .it[data-g="zoom"]').click();
  await expect(page.locator('[data-key="smoothZoomAccel"]')).toHaveCount(0);   // advanced is off
  await input(page).fill('soft start');
  const accel = page.locator('.res .hit[data-hit="smoothZoomAccel"]');
  await expect(accel).toBeVisible();
  await expect(page.locator('.res .hit[data-hit="smoothZoomRamp"]')).toBeVisible();
  const sl = accel.locator('input[type=range]');
  await sl.focus();
  await page.keyboard.press('ArrowRight');
  expect(await page.evaluate(() => window.__live.smoothZoomAccel)).toBe('3.5');
  // The switch stayed off and the page still hides the row.
  await page.locator('.side .it[data-g="prefs"]').click();
  await expect(page.locator('[data-key="showAdvanced"]').getByRole('switch')).not.toBeChecked();
  await page.locator('.side .it[data-g="zoom"]').click();
  await expect(page.locator('[data-key="smoothZoomAccel"]')).toHaveCount(0);
  // The engine row (advanced) is editable from search as well.
  await input(page).fill('engine');
  await expect(page.locator('.res .hit[data-hit="model"]')).toBeVisible();
});

test('a row gated by another setting shows dimmed, not hidden', async ({ page }) => {
  await page.goto('/');
  await input(page).fill('edge margin');
  const row = page.locator('.res .hit[data-hit="mouseMarginPct"] .row');
  await expect(row).toBeVisible();
  await expect(row).toHaveClass(/disabled/);   // mouseAlign is Centred
});

test('tray item lists never appear in results; the Performance switch does', async ({ page }) => {
  await page.goto('/');
  await page.locator('.side .it[data-g="tray"]').click();
  await expect(page.locator('main')).toContainText('Performance in the tray');
  await input(page).fill('tray');
  await expect(page.locator('.res .hit[data-hit="trayPerf"]')).toBeVisible();
  await expect(page.locator('.res .hit').filter({ has: page.locator('.drag, [draggable=true]') })).toHaveCount(0);
  const ids = await page.locator('.res .hit').evaluateAll((els) => els.map((e) => e.getAttribute('data-hit')));
  expect(ids.filter((k) => k.startsWith('tray') && k !== 'trayPerf')).toEqual([]);
  await input(page).fill('slider');
  await expect(page.locator('.res [data-hit^="tray"]:not([data-hit="trayPerf"])')).toHaveCount(0);
});

test('Esc clears, Ctrl+F focuses with a ring, Enter moves into the first result, Tab walks the controls', async ({ page }) => {
  await page.goto('/');
  await expect(page.locator('h1')).toHaveText('Hotkeys');
  await page.locator('main').click({ position: { x: 5, y: 5 } });
  await page.keyboard.press('Control+f');
  await expect(input(page)).toBeFocused();
  await expect(page.locator('.search')).toHaveCSS('border-top-color', 'rgb(242, 242, 242)');   // after its short fade
  await input(page).fill('speed');
  await expect(page.locator('.res')).toBeVisible();
  await input(page).press('Enter');
  const firstKey = keys('speed')[0];
  await expect(page.locator('.res .hit[data-hit="' + firstKey + '"] input').first()).toBeFocused();
  await page.keyboard.press('Tab');
  const secondKey = keys('speed')[1];
  await expect(page.locator('.res .hit[data-hit="' + secondKey + '"] input').first()).toBeFocused();
  await page.keyboard.press('Escape');
  await expect(page.locator('.res')).toHaveCount(0);
  await expect(input(page)).toHaveValue('');
  await expect(page.locator('h1')).toBeVisible();
});

test('mouse clicks on a result show no focus ring; keyboard focus does', async ({ page }) => {
  await page.goto('/');
  await input(page).fill('keyboard focus');
  const sw = page.locator('.res .hit[data-hit="trackFocus"] input[role=switch]');
  await sw.click({ force: true });
  const outline = (loc) => loc.evaluate((e) => getComputedStyle(e.nextElementSibling).outlineStyle);
  expect(await outline(sw)).toBe('none');
  await page.keyboard.press('Shift+Tab');
  await page.keyboard.press('Tab');
  expect(await outline(sw)).not.toBe('none');
});

test('no matches says so', async ({ page }) => {
  await page.goto('/');
  await input(page).fill('qqqq');
  await expect(page.locator('.res .sum')).toContainText('No settings match');
  await expect(page.locator('.res .hit')).toHaveCount(0);
});
