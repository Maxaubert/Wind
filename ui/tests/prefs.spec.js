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
          window.__live = { model: 'hybrid', uiTheme: window.__live.uiTheme }; window.__saved = { ...window.__live };
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

// ---- Mode -----------------------------------------------------------------------------------------

test('Mode is a three-way System / Light / Dark with equal segments and a clearly filled selected one', async ({ page }) => {
  await page.goto('/');
  await go(page, 'prefs');
  const seg = key(page, '__theme').getByRole('radiogroup');
  await expect(seg.getByRole('radio')).toHaveText(['System', 'Light', 'Dark']);
  const widths = [];
  for (const r of await seg.getByRole('radio').all()) widths.push(Math.round((await r.boundingBox()).width));
  expect(new Set(widths).size).toBe(1);   // equal-width segments
  // The ini says dark, so Dark is the selected one: filled with the selection colour, the others are not.
  const dark = seg.getByRole('radio', { name: 'Dark' });
  const sel = await token(page, 'sel');
  expect(await css(dark, 'background-color')).not.toBe(await css(seg.getByRole('radio', { name: 'Light' }), 'background-color'));
  expect(await css(dark, 'font-weight')).toBe('600');
  expect(sel).toBe('#313131');
});

test('Mode writes uiTheme auto / light / dark, and System follows the system setting', async ({ page }) => {
  await page.emulateMedia({ colorScheme: 'light' });
  await page.goto('/');
  await go(page, 'prefs');
  const seg = key(page, '__theme').getByRole('radiogroup');
  await seg.getByRole('radio', { name: 'System' }).click();
  await expect(app(page)).toHaveAttribute('data-theme', 'light');   // the system is light
  await page.emulateMedia({ colorScheme: 'dark' });
  await expect(app(page)).toHaveAttribute('data-theme', 'dark');   // and follows it live
  await seg.getByRole('radio', { name: 'Light' }).click();
  await expect(app(page)).toHaveAttribute('data-theme', 'light');
  await seg.getByRole('radio', { name: 'Dark' }).click();
  await expect(app(page)).toHaveAttribute('data-theme', 'dark');
  expect((await sent(page, 'setConfig')).filter((m) => m.key === 'uiTheme').map((m) => m.value)).toEqual(['auto', 'light', 'dark']);
  await expect(page.locator('.capsule')).toHaveCount(0);   // a mode change is never unsaved
});

// ---- Themes ---------------------------------------------------------------------------------------

test('the theme picker is one scrolling row of the eight themes', async ({ page }) => {
  await page.setViewportSize({ width: 820, height: 700 });
  await page.goto('/');
  await go(page, 'prefs');
  const picker = key(page, 'uiPalette').getByRole('radiogroup');
  await expect(picker.getByRole('radio')).toHaveText(themes.map((t) => t.label));
  expect(themes.map((t) => t.id)).toEqual(['grey', 'ember', 'cyber', 'mono', 'slate', 'carbon', 'hicon', 'ocean']);
  const tops = new Set();
  for (const r of await picker.getByRole('radio').all()) tops.add(Math.round((await r.boundingBox()).y));
  expect(tops.size).toBe(1);   // one row, no wrapping
  // Too narrow for all eight: the strip scrolls sideways instead of wrapping or clipping.
  expect(await picker.evaluate((el) => el.scrollWidth > el.clientWidth)).toBe(true);
  expect(await css(picker, 'overflow-x')).toBe('auto');
  await expect(picker.getByRole('radio', { checked: true })).toHaveText('Wind grey');
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
  // The mode picks the light block of the same theme.
  await key(page, '__theme').getByRole('radio', { name: 'Light' }).click();
  expect(await token(page, 'fill')).toBe('#1262b8');
  expect(await token(page, 'bg')).toBe('#eff3f8');
});

test('every theme has a dark and a light block that differ, and the saved theme loads at launch', async ({ page }) => {
  await page.addInitScript(() => { window.__cfgExtra = { uiPalette: 'carbon' }; });
  await page.goto('/');
  await expect(app(page)).toHaveAttribute('data-palette', 'carbon');
  await go(page, 'prefs');
  const picker = key(page, 'uiPalette').getByRole('radiogroup');
  for (const mode of ['Dark', 'Light']) {
    await key(page, '__theme').getByRole('radio', { name: mode }).click();
    const seen = new Set();
    for (const t of themes) {
      await picker.getByRole('radio', { name: t.label }).click();
      await expect(app(page)).toHaveAttribute('data-palette', t.id);
      seen.add([await token(page, 'bg'), await token(page, 'fill'), await token(page, 'card'), await token(page, 'sel'), await token(page, 'fg3')].join('|'));
    }
    expect(seen.size, mode + ' blocks are all distinct').toBe(themes.length);
  }
});

test('an unknown uiPalette falls back to Wind grey', async ({ page }) => {
  await page.addInitScript(() => { window.__cfgExtra = { uiPalette: 'nope' }; });
  await page.goto('/');
  await expect(app(page)).toHaveAttribute('data-palette', 'grey');
});

test('Wind grey is today\'s look: every token that existed before themes is unchanged, dark and light', async ({ browser }) => {
  const tokens = readFileSync(new URL('../src/design/tokens.css', import.meta.url), 'utf8');
  const names = [...tokens.slice(tokens.indexOf('.wnd {'), tokens.indexOf('.wnd[data-theme="light"]')).matchAll(/--([A-Za-z0-9-]+):/g)]
    .map((m) => m[1]).filter((n) => !['s', 'm', 'nf', 'w', 'isz', 'dur-fast', 'dur', 'ease', 'bandimg-from', 'rad', 'srad'].includes(n));
  const page = await browser.newPage();
  for (const mode of ['dark', 'light']) {
    const read = async (url) => {
      await page.goto(url);
      return page.evaluate((ns) => {
        const el = document.querySelector('.wnd');
        const cs = getComputedStyle(el);
        return Object.fromEntries(ns.map((n) => [n, cs.getPropertyValue('--' + n).trim().replace(/\s+/g, ' ')]));
      }, names);
    };
    const before = await read('/preview.html?theme=' + mode);                    // tokens.css alone, no palette
    const grey = await read('/controls.html?group=prefs&palette=grey&theme=' + mode);   // through themes.css
    for (const n of names) {
      // The new tokens have defaults in tokens.css, the old ones must match exactly.
      if (['sel', 'selfg', 'onfill', 'fillline', 'focus', 'hint', 'hover2', 'scrim', 'danger', 'dangerbg', 'dangerbtn', 'dangerbtnfg',
        'bnfilter', 'bntint', 'bntop', 'rc', 'rp', 'rsw', 'rkn'].includes(n)) continue;
      expect(grey[n], mode + ' --' + n).toBe(before[n]);
    }
  }
  await page.close();
});

test('Cyberpunk dark selections are solid #fcee0a with black text, and the sharp themes square the corners', async ({ page }) => {
  await page.addInitScript(() => { window.__cfgExtra = { uiPalette: 'cyber' }; });
  await page.goto('/');
  const sel = page.locator('.side .it.sel');
  await expect(sel).toContainText('Hotkeys');
  expect(await css(sel, 'background-color')).toBe('rgb(252, 238, 10)');
  expect(await css(sel, 'color')).toBe('rgb(0, 0, 0)');
  expect(await css(page.locator('.card').first(), 'border-radius')).toBe('4px');
  await go(page, 'prefs');
  const on = key(page, '__theme').getByRole('radio', { checked: true });
  expect(await css(on, 'background-color')).toBe('rgb(252, 238, 10)');
  expect(await css(on, 'color')).toBe('rgb(0, 0, 0)');
  for (const [id, rc] of [['carbon', '4px'], ['hicon', '4px'], ['grey', '10px'], ['ocean', '10px']]) {
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

test('New: the dialog suggests a name, checks it, and starts from the defaults', async ({ page }) => {
  await page.goto('/');
  await go(page, 'prefs');
  await key(page, '__profiles').getByRole('button', { name: 'New' }).click();
  const dlg = page.getByRole('dialog', { name: 'New profile' });
  const name = dlg.getByLabel('Name');
  await expect(name).toHaveValue('Profile 3');
  await expect(name).toBeFocused();
  const from = dlg.getByRole('radiogroup', { name: 'Start from' });
  await expect(from.getByRole('radio')).toHaveText(['Current settings (copy of Default)', 'Default settings']);
  await expect(from.getByRole('radio', { checked: true })).toHaveText(/Current settings/);
  await name.fill('gaming');
  await dlg.getByRole('button', { name: 'Create' }).click();
  await expect(dlg.getByRole('alert')).toHaveText('A profile with this name already exists.');
  await name.fill('');
  await dlg.getByRole('button', { name: 'Create' }).click();
  await expect(dlg.getByRole('alert')).toHaveText('Enter a name for the profile.');
  expect(await sent(page, 'createProfile')).toHaveLength(0);
  await name.fill('Fresh');
  await from.getByRole('radio', { name: 'Default settings' }).click();
  await dlg.getByRole('button', { name: 'Create' }).click();
  await expect(dlg).toHaveCount(0);
  await expect(trig(page)).toHaveText('Fresh');
  expect((await sent(page, 'createProfile')).map((m) => m.name)).toEqual(['Fresh']);
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
  await dlg.getByRole('button', { name: 'Create' }).click();
  await expect(page.getByRole('dialog')).toHaveCount(0);   // no Save / Discard question: the changes come along
  await expect(trig(page)).toHaveText('Copy');
  expect((await sent(page, 'createProfile')).map((m) => m.name)).toEqual(['Copy']);
  // The new profile holds them: persisted into the live ini AND the profile file, so nothing is unsaved.
  await expect.poll(async () => page.evaluate(() => [window.__live.maxLevel, window.__saved.maxLevel])).toEqual(['20', '20']);
  await expect(page.locator('.capsule')).toHaveCount(0);
  expect((await sent(page, 'setConfigPersist')).some((m) => m.key === 'maxLevel' && m.value === '20')).toBe(true);
  // Global keys do not travel as profile data.
  expect((await sent(page, 'setConfigPersist')).some((m) => m.key === 'uiTheme' || m.key === 'uiPalette')).toBe(false);
});

test('New from the defaults with unsaved changes asks Save / Discard first', async ({ page }) => {
  await page.goto('/');
  await go(page, 'zoom');
  await page.locator('[data-key="maxLevel"] input[type=range]').fill('20');
  await go(page, 'prefs');
  await key(page, '__profiles').getByRole('button', { name: 'New' }).click();
  const dlg = page.getByRole('dialog', { name: 'New profile' });
  await dlg.getByLabel('Name').fill('Fresh');
  await dlg.getByRole('radio', { name: 'Default settings' }).click();
  await dlg.getByRole('button', { name: 'Create' }).click();
  const ask = page.getByRole('dialog', { name: 'Unsaved changes' });
  await expect(ask).toBeVisible();
  await ask.getByRole('button', { name: 'Discard' }).click();
  await expect.poll(async () => (await sent(page, 'createProfile')).length).toBe(1);
});

test('the dialogs are keyboard friendly: Escape closes, Enter creates, focus returns to New', async ({ page }) => {
  await page.goto('/');
  await go(page, 'prefs');
  const newBtn = key(page, '__profiles').getByRole('button', { name: 'New' });
  await newBtn.click();
  await page.keyboard.press('Escape');
  await expect(page.getByRole('dialog')).toHaveCount(0);
  await expect(newBtn).toBeFocused();
  await newBtn.click();
  await page.getByLabel('Name').fill('Quick');
  await page.keyboard.press('Enter');
  await expect(trig(page)).toHaveText('Quick');
});

test('Troubleshooting is always there, and the advanced switch is the last row of General', async ({ page }) => {
  await page.goto('/');
  await go(page, 'prefs');
  await expect(page.locator('main .cap')).toHaveText(['General', 'Troubleshooting']);
  await expect(page.locator('main .card').first().locator('.row .label')).toHaveText(['Mode', 'Theme', 'Profile', 'Show advanced settings']);
  await expect(page.locator('main .card').nth(1).locator('.row .label')).toHaveText(['Frame time logging', 'Export diagnostics', 'Open settings file']);
});

test('prefs files contain no em-dash', () => {
  const dash = String.fromCharCode(0x2014);
  for (const f of ['../src/prefs/ThemePicker.svelte', '../src/prefs/ProfilePicker.svelte', '../src/prefs/NewProfileDialog.svelte',
    '../src/prefs/profileName.js', '../src/controls/ModeSwitch.svelte', '../src/design/themes.css', '../src/design/themes.js',
    '../tools/gen-themes.cjs'])
    expect(readFileSync(new URL(f, import.meta.url), 'utf8'), f).not.toContain(dash);
});
