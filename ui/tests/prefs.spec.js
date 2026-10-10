// Preferences page (#318, Task 4): the Mode switch, the built-in themes (tokens change with uiPalette and the
// resolved mode) and the profile dropdown with its New and delete dialogs, against a mock host that
// implements the session and profile messages.
import { test, expect } from '@playwright/test';
import { readFileSync } from 'node:fs';
import { themes } from '../src/design/themes.js';

test.beforeEach(async ({ page }) => {
  await page.addInitScript(() => {
    window.__skipSplash = true;
    window.__msgs = [];
    const init = () => {
      if (window.__live) return;
      window.__live = { maxLevel: '12', model: 'hybrid', uiTheme: 'dark', ...(window.__cfgExtra || {}) };
      window.__saved = { ...window.__live };
      window.__profiles = { names: window.__names || ['Default', 'Gaming'], active: 'Default' };
    };
    const listeners = new Set();
    const send = (data) => listeners.forEach((fn) => fn({ data }));
    window.__hostSend = send;
    const cfg = () => ({ type: 'config', values: { ...window.__live }, saved: { ...window.__saved },
                         profiles: { names: [...window.__profiles.names], active: window.__profiles.active } });
    const reply = (ok = true, error = '') => send({ type: 'profiles', names: [...window.__profiles.names], active: window.__profiles.active, ok, error });
    window.chrome = { webview: {
      addEventListener: (_e, fn) => listeners.add(fn),
      postMessage: (msg) => {
        window.__msgs.push(msg);
        init();
        if (msg.type === 'getConfig') send(cfg());
        else if (msg.type === 'setConfig') window.__live[msg.key] = msg.value;
        else if (msg.type === 'setConfigPersist') { window.__live[msg.key] = msg.value; window.__saved[msg.key] = msg.value; }
        else if (msg.type === 'saveSession') {
          for (const k of Object.keys(window.__live)) if (k !== 'uiTheme') window.__saved[k] = window.__live[k];
          send({ type: 'sessionSaved', ok: true });
        } else if (msg.type === 'discardSession') {
          for (const k of Object.keys(window.__saved)) window.__live[k] = window.__saved[k];
          send(cfg());
        } else if (msg.type === 'mpoState') send({ type: 'mpoState', disabled: false, bootKnown: true, atBoot: false });
        else if (msg.type === 'switchProfile') { window.__profiles.active = msg.name; reply(); }
        else if (msg.type === 'createProfile') {
          // The host starts a new profile from the defaults (globals carry over) and switches to it.
          window.__profiles.names.push(msg.name); window.__profiles.active = msg.name;
          // fromCurrent: the host builds the profile from the live session, unsaved changes included.
          if (msg.fromCurrent === '1') window.__saved = { ...window.__live };
          else { window.__live = { model: 'hybrid', uiTheme: window.__live.uiTheme }; window.__saved = { ...window.__live }; }
          reply();
        } else if (msg.type === 'deleteProfile') {
          window.__profiles.names = window.__profiles.names.filter((n) => n !== msg.name);
          if (window.__profiles.active === msg.name) window.__profiles.active = window.__profiles.names[0];
          reply();
        }
      },
    } };
  });
});

const sent = (page, type) => page.evaluate((t) => window.__msgs.filter((m) => m.type === t), type);
const go = (page, g) => page.locator('.side .it[data-g="' + g + '"]').click();
const css = (loc, prop) => loc.evaluate((el, p) => getComputedStyle(el).getPropertyValue(p), prop);
const key = (page, k) => page.locator('[data-key="' + k + '"]');
const app = (page) => page.locator('.wnd.app');
const token = async (page, name) => (await css(app(page), '--' + name)).trim();
const trig = (page) => key(page, '__profiles').locator('.trig');
const openList = (page) => trig(page).click();

// ---- Dark only (#324) -----------------------------------------------------------------------------

test('there is no Mode row, and an ini with uiTheme=light or auto still renders dark', async ({ page }) => {
  for (const legacy of ['light', 'auto', 'dark']) {
    await page.addInitScript((v) => { window.__cfgExtra = { uiTheme: v }; }, legacy);
    await page.emulateMedia({ colorScheme: 'light' });   // the system being light changes nothing either
    await page.goto('/');
    await go(page, 'prefs');
    await expect(key(page, '__theme')).toHaveCount(0);
    await expect(page.getByRole('radio', { name: 'Light' })).toHaveCount(0);
    expect(await token(page, 'bg'), legacy).toBe('#000');
    expect(await css(page.locator('html'), 'color-scheme'), legacy).toBe('dark');
    expect(await page.evaluate(() => document.querySelector('.wnd').hasAttribute('data-theme'))).toBe(false);
    expect((await sent(page, 'setConfig')).filter((m) => m.key === 'uiTheme')).toEqual([]);   // the UI never writes it any more
  }
});

// ---- Themes ---------------------------------------------------------------------------------------

test('the theme picker is one row of the four theme cards, no scrolling, no arrows', async ({ page }) => {
  await page.setViewportSize({ width: 820, height: 700 });
  await page.goto('/');
  await go(page, 'prefs');
  const picker = key(page, 'uiPalette').getByRole('radiogroup');
  const radios = picker.getByRole('radio');
  await expect(radios).toHaveCount(themes.length);
  for (let i = 0; i < themes.length; i++) await expect(radios.nth(i)).toHaveAccessibleName(themes[i].label);
  await expect(radios).toHaveText(themes.map(() => ''));   // no visible names, cards only
  expect(themes.map((t) => t.id)).toEqual(['grey', 'ember', 'ocean', 'hicon']);   // High contrast is always last
  const tops = new Set();
  for (const r of await picker.getByRole('radio').all()) tops.add(Math.round((await r.boundingBox()).y));
  expect(tops.size).toBe(1);   // one row, no wrapping
  // Four fit: nothing scrolls, there are no arrow buttons and no edge fade mask.
  expect(await picker.evaluate((el) => el.scrollWidth <= el.clientWidth)).toBe(true);
  expect(await css(picker, 'overflow-x')).toBe('visible');
  await expect(key(page, 'uiPalette').locator('button')).toHaveCount(4);   // the four cards only
  expect(await css(picker, 'mask-image')).toBe('none');
  // At this width the row stacks (control under the text): the last card stays inside the row.
  const rowBox = await key(page, 'uiPalette').boundingBox();
  const lastBox = await picker.getByRole('radio').last().boundingBox();
  expect(lastBox.x + lastBox.width).toBeLessThanOrEqual(rowBox.x + rowBox.width);
  await expect(picker.getByRole('radio', { checked: true })).toHaveAccessibleName('Wind grey');
  await expect(picker.locator('.nm')).toHaveCount(0);   // cards only, no names (owner decision)
  // Each card is drawn in its own theme: the mini windows differ.
  const bgs = new Set();
  for (const sw of await picker.locator('.sw').all()) bgs.add((await css(sw, 'background-color')) + '|' + (await css(sw.locator('.ac'), 'background-color')));
  expect(bgs.size).toBe(4);
});

test('theme picker keyboard: Left/Right moves and applies, the focus ring is for the keyboard only', async ({ page }) => {
  await page.goto('/');
  await go(page, 'prefs');
  const picker = key(page, 'uiPalette').getByRole('radiogroup');
  await picker.getByRole('radio', { name: 'Wind grey' }).click();
  expect(await css(picker.getByRole('radio', { name: 'Wind grey' }), 'outline-style')).toBe('none');   // a click leaves no ring
  await page.keyboard.press('ArrowRight');
  await expect(app(page)).toHaveAttribute('data-palette', 'ember');
  await expect(picker.getByRole('radio', { name: 'Ember' })).toBeFocused();
  expect(await css(picker.getByRole('radio', { name: 'Ember' }), 'outline-style')).not.toBe('none');
  await page.keyboard.press('ArrowLeft');
  await expect(app(page)).toHaveAttribute('data-palette', 'grey');
  await page.keyboard.press('End');
  await expect(app(page)).toHaveAttribute('data-palette', 'hicon');
  await page.keyboard.press('ArrowRight');   // wraps to the first
  await expect(app(page)).toHaveAttribute('data-palette', 'grey');
});

test('picking a theme restyles the window, writes uiPalette and is never unsaved', async ({ page }) => {
  await page.goto('/');
  await go(page, 'prefs');
  await expect(app(page)).toHaveAttribute('data-palette', 'grey');
  expect(await token(page, 'fill')).toBe('#2fbfa5');
  const picker = key(page, 'uiPalette').getByRole('radiogroup');
  await picker.getByRole('radio', { name: 'Ember' }).click();
  await expect(app(page)).toHaveAttribute('data-palette', 'ember');
  expect(await token(page, 'fill')).toBe('#e48a45');
  expect(await token(page, 'bg')).toBe('#0c0908');
  expect(await css(page.locator('.card').first(), 'background-color')).toBe('rgb(19, 14, 11)');
  await picker.getByRole('radio', { name: 'Deep ocean' }).click();
  expect(await token(page, 'fill')).toBe('#5aaaf5');
  expect((await sent(page, 'setConfig')).filter((m) => m.key === 'uiPalette').map((m) => m.value)).toEqual(['ember', 'ocean']);
  await expect(page.locator('.capsule')).toHaveCount(0);
});

test('every theme has its own block, and the saved theme loads at launch', async ({ page }) => {
  await page.addInitScript(() => { window.__cfgExtra = { uiPalette: 'hicon' }; });
  await page.goto('/');
  await expect(app(page)).toHaveAttribute('data-palette', 'hicon');
  await go(page, 'prefs');
  const picker = key(page, 'uiPalette').getByRole('radiogroup');
  {
    const seen = new Set();
    for (const t of themes) {
      await picker.getByRole('radio', { name: t.label }).click();
      await expect(app(page)).toHaveAttribute('data-palette', t.id);
      seen.add([await token(page, 'bg'), await token(page, 'fill'), await token(page, 'card'), await token(page, 'sel'), await token(page, 'fg3')].join('|'));
    }
    expect(seen.size, 'theme blocks are all distinct').toBe(themes.length);
  }
});

test('an unknown uiPalette falls back to Wind grey', async ({ page }) => {
  await page.addInitScript(() => { window.__cfgExtra = { uiPalette: 'nope' }; });
  await page.goto('/');
  await expect(app(page)).toHaveAttribute('data-palette', 'grey');
});

test('Wind grey is today\'s look: every token that existed before themes is unchanged', async ({ browser }) => {
  const tokens = readFileSync(new URL('../src/design/tokens.css', import.meta.url), 'utf8');
  const names = [...tokens.slice(tokens.indexOf('.wnd {'), tokens.indexOf('\n}', tokens.indexOf('.wnd {'))).matchAll(/--([A-Za-z0-9-]+):/g)]
    .map((m) => m[1]).filter((n) => !['s', 'm', 'nf', 'w', 'isz', 'dur-fast', 'dur', 'ease', 'bandimg-from', 'rad', 'srad'].includes(n));
  const page = await browser.newPage();
  {
    const read = async (url) => {
      await page.goto(url);
      return page.evaluate((ns) => {
        const el = document.querySelector('.wnd');
        const cs = getComputedStyle(el);
        return Object.fromEntries(ns.map((n) => [n, cs.getPropertyValue('--' + n).trim().replace(/\s+/g, ' ')]));
      }, names);
    };
    const before = await read('/preview.html');   // tokens.css alone, no palette
    const grey = await read('/controls.html?group=prefs&palette=grey');   // through themes.css
    for (const n of names) {
      // The new tokens have defaults in tokens.css, the old ones must match exactly.
      if (['sel', 'selfg', 'onfill', 'fillline', 'focus', 'hint', 'hover2', 'scrim', 'danger', 'dangerbg', 'dangerbtn', 'dangerbtnfg',
        'bnfilter', 'bntint', 'bntop', 'rc', 'rp', 'rsw', 'rkn'].includes(n)) continue;
      expect(grey[n], '--' + n).toBe(before[n]);
    }
  }
  await page.close();
});

test('a removed theme id reads as Wind grey, and the sharp radii belong to High contrast only', async ({ page }) => {
  for (const gone of ['cyber', 'mono', 'slate', 'carbon']) {
    await page.addInitScript((id) => { window.__cfgExtra = { uiPalette: id }; }, gone);
    await page.goto('/');
    await expect(app(page), gone).toHaveAttribute('data-palette', 'grey');
  }
  await page.goto('/');
  await go(page, 'prefs');
  for (const [id, rc] of [['hicon', '4px'], ['grey', '10px'], ['ocean', '10px'], ['ember', '10px']]) {
    await key(page, 'uiPalette').getByRole('radio', { name: themes.find((t) => t.id === id).label }).click();
    expect(await css(page.locator('.card').first(), 'border-radius'), id).toBe(rc);
  }
});

// ---- Profiles -------------------------------------------------------------------------------------

test('the profile row is a dropdown plus New; the open list has a trash per deletable profile', async ({ page }) => {
  await page.addInitScript(() => { window.__names = ['Default', 'Gaming', 'Work']; });
  await page.goto('/');
  await go(page, 'prefs');
  const row = key(page, '__profiles');
  await expect(row.getByRole('button')).toHaveText(['Default', 'New']);   // nothing else on the row
  await expect(row.getByRole('listbox')).toHaveCount(0);
  await openList(page);
  await expect(row.getByRole('option')).toHaveText(['Default', 'Gaming', 'Work']);
  await expect(row.getByRole('option', { selected: true })).toHaveText('Default');
  await expect(row.getByRole('button', { name: /^Delete profile / })).toHaveCount(2);   // not on Default (host-protected)
  await page.keyboard.press('Escape');
  await expect(row.getByRole('listbox')).toHaveCount(0);
});

test('with one profile left there is no trash at all', async ({ page }) => {
  await page.addInitScript(() => { window.__names = ['Default']; });
  await page.goto('/');
  await go(page, 'prefs');
  await openList(page);
  await expect(key(page, '__profiles').getByRole('option')).toHaveCount(1);
  await expect(key(page, '__profiles').getByRole('button', { name: /^Delete profile / })).toHaveCount(0);
});

test('choosing a profile in the list switches to it', async ({ page }) => {
  await page.goto('/');
  await go(page, 'prefs');
  await openList(page);
  await page.getByRole('option', { name: 'Gaming' }).click();
  await expect(trig(page)).toHaveText('Gaming');
  expect((await sent(page, 'switchProfile')).map((m) => m.name)).toEqual(['Gaming']);
});

test('trash asks to confirm: Cancel keeps the profile, Delete removes it', async ({ page }) => {
  await page.addInitScript(() => { window.__names = ['Default', 'Gaming', 'Work']; });
  await page.goto('/');
  await go(page, 'prefs');
  await openList(page);
  await page.getByRole('button', { name: 'Delete profile Gaming' }).click();
  const dlg = page.getByRole('dialog', { name: 'Delete profile' });
  await expect(dlg).toContainText('Delete "Gaming"? This cannot be undone.');
  await expect(dlg.getByRole('button')).toHaveText(['Cancel', 'Delete']);
  await dlg.getByRole('button', { name: 'Cancel' }).click();
  expect(await sent(page, 'deleteProfile')).toHaveLength(0);
  await openList(page);
  await page.getByRole('button', { name: 'Delete profile Gaming' }).click();
  await page.getByRole('dialog').getByRole('button', { name: 'Delete' }).click();
  await expect.poll(async () => (await sent(page, 'deleteProfile')).map((m) => m.name)).toEqual(['Gaming']);
  await openList(page);
  await expect(page.getByRole('option')).toHaveText(['Default', 'Work']);
});

test('New: the dialog suggests a name, checks it, and New starts from the defaults', async ({ page }) => {
  await page.goto('/');
  await go(page, 'prefs');
  await key(page, '__profiles').getByRole('button', { name: 'New' }).click();
  const dlg = page.getByRole('dialog', { name: 'New profile' });
  const name = dlg.getByLabel('Name');
  await expect(name).toHaveValue('Profile 3');
  await expect(name).toBeFocused();
  // Three buttons, no "Start from" choice (owner decision): Cancel, New, Duplicate current.
  await expect(dlg.getByRole('button')).toHaveText(['Cancel', 'New', 'Duplicate current']);
  await expect(dlg.getByRole('radio')).toHaveCount(0);
  await name.fill('gaming');
  await dlg.getByRole('button', { name: 'New', exact: true }).click();
  await expect(dlg.getByRole('alert')).toHaveText('A profile with this name already exists.');
  await expect(name).toBeFocused();
  await name.fill('');
  await dlg.getByRole('button', { name: 'Duplicate current' }).click();
  await expect(dlg.getByRole('alert')).toHaveText('Enter a name for the profile.');
  expect(await sent(page, 'createProfile')).toHaveLength(0);
  await name.fill('Fresh');
  await dlg.getByRole('button', { name: 'New', exact: true }).click();
  await expect(dlg).toHaveCount(0);
  await expect(trig(page)).toHaveText('Fresh');
  expect((await sent(page, 'createProfile')).map((m) => [m.name, m.fromCurrent])).toEqual([['Fresh', '0']]);
  expect(await sent(page, 'setConfigPersist')).toHaveLength(0);   // defaults: nothing copied in
});

test('New from the current settings copies them into the new profile, even with unsaved changes', async ({ page }) => {
  await page.goto('/');
  await go(page, 'zoom');
  await page.locator('[data-key="maxLevel"] input[type=range]').fill('20');
  await expect(page.locator('.capsule')).toContainText('1 unsaved change');
  await go(page, 'prefs');
  await key(page, '__profiles').getByRole('button', { name: 'New' }).click();
  const dlg = page.getByRole('dialog', { name: 'New profile' });
  await dlg.getByLabel('Name').fill('Copy');
  await dlg.getByRole('button', { name: 'Duplicate current' }).click();
  await expect(page.getByRole('dialog')).toHaveCount(0);   // no Save / Discard question: the changes come along
  await expect(trig(page)).toHaveText('Copy');
  // The host builds the profile from the live session (fromCurrent), so nothing is written back from
  // the page: no model patch, no per-key persist, and the outgoing profile is never mirrored into.
  expect((await sent(page, 'createProfile')).map((m) => [m.name, m.fromCurrent])).toEqual([['Copy', '1']]);
  await expect.poll(async () => page.evaluate(() => [window.__live.maxLevel, window.__saved.maxLevel])).toEqual(['20', '20']);
  await expect(page.locator('.capsule')).toHaveCount(0);
  expect(await sent(page, 'setConfigPersist')).toHaveLength(0);
});

test('New from the defaults with unsaved changes asks Save / Discard first', async ({ page }) => {
  await page.goto('/');
  await go(page, 'zoom');
  await page.locator('[data-key="maxLevel"] input[type=range]').fill('20');
  await go(page, 'prefs');
  await key(page, '__profiles').getByRole('button', { name: 'New' }).click();
  const dlg = page.getByRole('dialog', { name: 'New profile' });
  await dlg.getByLabel('Name').fill('Fresh');
  await dlg.getByRole('button', { name: 'New', exact: true }).click();
  const ask = page.getByRole('dialog', { name: 'Unsaved changes' });
  await expect(ask).toBeVisible();
  await ask.getByRole('button', { name: 'Discard' }).click();
  await expect.poll(async () => (await sent(page, 'createProfile')).length).toBe(1);
});

test('the dialogs are keyboard friendly: Escape closes, Enter duplicates, focus returns to New', async ({ page }) => {
  await page.goto('/');
  await go(page, 'prefs');
  const newBtn = key(page, '__profiles').getByRole('button', { name: 'New' });
  await newBtn.click();
  await page.keyboard.press('Escape');
  await expect(page.getByRole('dialog')).toHaveCount(0);
  await expect(newBtn).toBeFocused();
  await newBtn.click();
  await page.getByLabel('Name').fill('Quick');
  await page.keyboard.press('Enter');   // Enter in the name field = Duplicate current, the old default
  await expect(trig(page)).toHaveText('Quick');
  await expect(page.locator('[role=status][aria-live=polite]')).toContainText('Created profile Quick from the current settings');
});

test('New profile: Tab walks name, Cancel, New, Duplicate current, and Cancel creates nothing', async ({ page }) => {
  await page.goto('/');
  await go(page, 'prefs');
  await key(page, '__profiles').getByRole('button', { name: 'New' }).click();
  const dlg = page.getByRole('dialog', { name: 'New profile' });
  await expect(dlg.getByLabel('Name')).toBeFocused();
  await page.keyboard.press('Tab');
  await expect(dlg.getByRole('button', { name: 'Cancel' })).toBeFocused();
  await page.keyboard.press('Tab');
  await expect(dlg.getByRole('button', { name: 'New', exact: true })).toBeFocused();
  await page.keyboard.press('Tab');
  await expect(dlg.getByRole('button', { name: 'Duplicate current' })).toBeFocused();
  // The buttons sit on one row inside the dialog (measured once the open animation is done).
  await dlg.evaluate((el) => Promise.all(el.getAnimations().map((a) => a.finished)));
  const box = await dlg.boundingBox();
  const tops = new Set();
  for (const b of await dlg.getByRole('button').all()) {
    const r = await b.boundingBox();
    tops.add(Math.round(r.y));
    expect(r.x + r.width).toBeLessThanOrEqual(box.x + box.width);
  }
  expect(tops.size).toBe(1);
  await dlg.getByRole('button', { name: 'Cancel' }).click();
  await expect(page.getByRole('dialog')).toHaveCount(0);
  expect(await sent(page, 'createProfile')).toHaveLength(0);
});

test('Troubleshooting is always there, and the advanced switch is the last row of General', async ({ page }) => {
  await page.goto('/');
  await go(page, 'prefs');
  await expect(page.locator('main .cap')).toHaveText(['General', 'Screen light', 'Troubleshooting']);
  await expect(page.locator('main .card').first().locator('.row .label')).toHaveText(['Theme', 'Profile', 'Pin to taskbar', 'Show advanced settings']);
  await expect(page.locator('main .card').nth(1).locator('.row .label')).toHaveText(['Warmth', 'Brightness']);
  await expect(page.locator('main .card').nth(2).locator('.row .label')).toHaveText(['Frame time logging', 'Export diagnostics', 'Open settings file']);
});

test('Pin to taskbar (#436): off by default, writes trayPinned, never an unsaved change', async ({ page }) => {
  await page.goto('/');
  await go(page, 'prefs');
  const row = key(page, 'trayPinned');
  await expect(row.locator('.label')).toHaveText('Pin to taskbar');
  await expect(row.locator('.desc')).toHaveText('Keeps the Wind icon next to the clock');
  const sw = row.getByRole('switch');
  await expect(sw).not.toBeChecked();   // ships off: the icon stays in the overflow
  await sw.check({ force: true });
  expect((await sent(page, 'setConfig')).filter((m) => m.key === 'trayPinned').map((m) => m.value)).toEqual(['1']);
  await sw.uncheck({ force: true });
  expect((await sent(page, 'setConfig')).filter((m) => m.key === 'trayPinned').map((m) => m.value)).toEqual(['1', '0']);
  // A global key: flipping it must not light up the unsaved-changes state.
  await sw.check({ force: true });
  await expect(page.locator('.capsule')).toHaveCount(0);
});

test('prefs files contain no em-dash', () => {
  const dash = String.fromCharCode(0x2014);
  for (const f of ['../src/prefs/ThemePicker.svelte', '../src/prefs/ProfilePicker.svelte', '../src/prefs/NewProfileDialog.svelte',
    '../src/prefs/profileName.js', '../src/design/themes.css', '../src/design/themes.js',
    '../tools/gen-themes.cjs'])
    expect(readFileSync(new URL(f, import.meta.url), 'utf8'), f).not.toContain(dash);
});
