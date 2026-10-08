import { test, expect } from '@playwright/test';
import { readFileSync } from 'node:fs';
import { changedKeys, fill, GLOBAL_KEYS } from '../src/session.js';

// Mock host for the session model: window.__live is the live ini, window.__saved the active profile.
// Every message the page posts lands in window.__msgs so tests can assert on order and content.
test.beforeEach(async ({ page }) => {
  await page.addInitScript(() => {
    window.__msgs = [];
    window.__live = { maxLevel: '12', zoomInVk: '33', zoomInButton: '2', model: 'hybrid', uiTheme: 'dark', ...(window.__liveExtra || {}) };
    window.__saved = { ...window.__live };
    window.__profiles = { names: ['Default', 'Gaming'], active: 'Default' };
    const listeners = new Set();
    const send = (data) => listeners.forEach((fn) => fn({ data }));
    window.__hostSend = send;
    const cfg = () => ({ type: 'config', values: { ...window.__live }, saved: { ...window.__saved },
                         profiles: { ...window.__profiles, names: [...window.__profiles.names] } });
    const reply = (ok = true) => send({ type: 'profiles', names: [...window.__profiles.names], active: window.__profiles.active, ok, error: '' });
    window.chrome = { webview: {
      addEventListener: (_e, fn) => listeners.add(fn),
      postMessage: (msg) => {
        window.__msgs.push(msg);
        if (msg.type === 'getConfig') send(cfg());
        else if (msg.type === 'setConfig') window.__live[msg.key] = msg.value;
        else if (msg.type === 'setConfigPersist') { window.__live[msg.key] = msg.value; window.__saved[msg.key] = msg.value; }
        else if (msg.type === 'saveSession') {
          for (const k of Object.keys(window.__live)) if (k !== 'uiTheme') window.__saved[k] = window.__live[k];
          send({ type: 'sessionSaved', ok: window.__saveOk !== false });
        } else if (msg.type === 'discardSession') {
          for (const k of Object.keys(window.__saved)) window.__live[k] = window.__saved[k];
          send(cfg());
        } else if (msg.type === 'mpoState') send({ type: 'mpoState', disabled: false, bootKnown: true, atBoot: false });
        else if (msg.type === 'listProfiles') reply();
        else if (msg.type === 'switchProfile') { window.__profiles.active = msg.name; reply(); }
      },
    } };
  });
});

const sent = (page, type) => page.evaluate((t) => window.__msgs.filter((m) => m.type === t), type);
const maxLevel = (page) => page.locator('[data-key="maxLevel"] input[type=range]');
const go = (page, g) => page.locator('.side .it[data-g="' + g + '"]').click();
const capsule = (page) => page.locator('.capsule');
const pickProfile = async (page, name) => {   // open the profile dropdown, choose a profile
  await page.locator('[data-key="__profiles"] .trig').click();
  await page.getByRole('option', { name }).click();
};

test('session helpers: global keys never count, defaults fill both sides', () => {
  expect(GLOBAL_KEYS.has('uiTheme')).toBe(true);
  expect(GLOBAL_KEYS.has('uiPalette')).toBe(true);   // #318: the built-in theme is global, UI-only
  expect(changedKeys({ a: '1', uiTheme: 'dark' }, { a: '1', uiTheme: 'light' })).toEqual([]);
  expect(changedKeys({ a: '2 ' }, { a: '2' })).toEqual([]);
  expect(changedKeys({ a: '2', b: '1' }, { a: '1', b: '1' })).toEqual(['a']);
  const f = fill({ values: { maxLevel: '20' }, saved: { maxLevel: '12' } });
  expect(f.values.zoomInVk).toBe('0');
  expect(f.saved.zoomInVk).toBe('0');
  expect(changedKeys(f.values, f.saved)).toEqual(['maxLevel']);
});

test('session files contain no em-dash', () => {
  for (const f of ['../src/session.js', '../src/Settings.svelte', '../src/prompts/Prompt.svelte', '../src/bridge.js'])
    expect(readFileSync(new URL(f, import.meta.url), 'utf8')).not.toContain(String.fromCharCode(0x2014));
});

test('opens clean: no capsule and the host is told nothing is dirty', async ({ page }) => {
  await page.goto('/');
  await expect(page.locator('h1')).toHaveText('Hotkeys');
  await expect(capsule(page)).toHaveCount(0);
  expect((await sent(page, 'dirty')).every((m) => m.value === '0')).toBe(true);
});

test('a change applies at once (setConfig, no Save) and raises the capsule; undoing it clears it', async ({ page }) => {
  await page.goto('/');
  await go(page, 'zoom');
  await maxLevel(page).fill('20');
  expect((await sent(page, 'setConfig')).some((m) => m.key === 'maxLevel' && m.value === '20')).toBe(true);
  expect(await sent(page, 'saveSession')).toHaveLength(0);
  await expect(capsule(page)).toContainText('1 unsaved change');
  await expect.poll(async () => (await sent(page, 'dirty')).at(-1)?.value).toBe('1');
  await maxLevel(page).fill('12');
  await expect(capsule(page)).toHaveCount(0);
});

test('Save posts saveSession and clears the capsule', async ({ page }) => {
  await page.goto('/');
  await go(page, 'zoom');
  await maxLevel(page).fill('20');
  await capsule(page).getByRole('button', { name: 'Save' }).click();
  await expect(capsule(page)).toHaveCount(0);
  expect(await sent(page, 'saveSession')).toHaveLength(1);
  expect(await page.evaluate(() => window.__saved.maxLevel)).toBe('20');
});

test('a failed Save keeps the capsule and says so', async ({ page }) => {
  await page.addInitScript(() => { window.__saveOk = false; });
  await page.goto('/');
  await go(page, 'zoom');
  await maxLevel(page).fill('20');
  await capsule(page).getByRole('button', { name: 'Save' }).click();
  await expect(page.getByRole('dialog', { name: "Couldn't save" })).toBeVisible();
  await expect(capsule(page)).toContainText('1 unsaved change');
});

test('Discard posts discardSession and reloads the saved values', async ({ page }) => {
  await page.goto('/');
  await go(page, 'zoom');
  await maxLevel(page).fill('20');
  await capsule(page).getByRole('button', { name: 'Discard' }).click();
  await expect(capsule(page)).toHaveCount(0);
  expect(await sent(page, 'discardSession')).toHaveLength(1);
  await expect(maxLevel(page)).toHaveValue('12');
});

test('keybind capture persists at once, never counts as unsaved, and survives Save', async ({ page }) => {
  await page.goto('/');
  await go(page, 'zoom');
  await maxLevel(page).fill('20');   // an unsaved change is pending while the bind is captured
  await go(page, 'hotkeys');
  await page.locator('[data-key="__zoomOut"] .kc.ghost').click();
  await page.keyboard.press('F2');
  const persisted = (await sent(page, 'setConfigPersist')).map((m) => m.key);
  expect(persisted).toContain('zoomOutVk');
  expect(await sent(page, 'setConfig').then((a) => a.some((m) => m.key === 'zoomOutVk'))).toBe(false);
  await expect(capsule(page)).toContainText('1 unsaved change');   // only the slider
  await capsule(page).getByRole('button', { name: 'Save' }).click();
  expect(await page.evaluate(() => window.__saved.zoomOutVk)).toBe('113');
});

test('closing with unsaved changes offers Save, Discard and Keep for this session', async ({ page }) => {
  await page.goto('/');
  await go(page, 'zoom');
  await maxLevel(page).fill('20');
  await page.getByRole('button', { name: 'Close' }).click();
  const dlg = page.getByRole('dialog', { name: 'Unsaved changes' });
  await expect(dlg.getByRole('button')).toHaveText(['Keep for this session', 'Discard', 'Save']);
  await dlg.getByRole('button', { name: 'Keep for this session' }).click();
  const win = (await sent(page, 'window')).filter((m) => m.action === 'close');
  expect(win).toEqual([{ type: 'window', action: 'close', force: '1' }]);
  expect(await sent(page, 'saveSession')).toHaveLength(0);
  expect(await sent(page, 'discardSession')).toHaveLength(0);
});

test('close prompt: Save saves then closes; Discard discards then closes; Esc cancels', async ({ page }) => {
  await page.goto('/');
  await go(page, 'zoom');
  await maxLevel(page).fill('20');
  await page.getByRole('button', { name: 'Close' }).click();
  await page.keyboard.press('Escape');
  await expect(page.getByRole('dialog')).toHaveCount(0);
  expect((await sent(page, 'window')).filter((m) => m.action === 'close')).toHaveLength(0);
  await page.getByRole('button', { name: 'Close' }).click();
  await page.getByRole('dialog').getByRole('button', { name: 'Save' }).click();
  await expect.poll(async () => (await sent(page, 'window')).filter((m) => m.action === 'close').length).toBe(1);
  const order = await page.evaluate(() => window.__msgs.map((m) => m.type).filter((t) => t === 'saveSession' || t === 'window'));
  expect(order.at(-2)).toBe('saveSession');
  expect(order.at(-1)).toBe('window');
});

test('closing with nothing unsaved does not prompt', async ({ page }) => {
  await page.goto('/');
  await page.getByRole('button', { name: 'Close' }).click();
  await expect(page.getByRole('dialog')).toHaveCount(0);
  expect((await sent(page, 'window')).filter((m) => m.action === 'close')).toEqual([{ type: 'window', action: 'close', force: '0' }]);
});

test('the host bouncing WM_CLOSE (confirmClose) raises the same prompt', async ({ page }) => {
  await page.goto('/');
  await go(page, 'zoom');
  await maxLevel(page).fill('20');
  await page.evaluate(() => window.__hostSend({ type: 'confirmClose' }));
  await expect(page.getByRole('dialog', { name: 'Unsaved changes' })).toBeVisible();
});

test('Quit Wind (Ctrl+Q) with unsaved changes asks Save / Discard / Cancel', async ({ page }) => {
  await page.goto('/');
  await go(page, 'zoom');
  await maxLevel(page).fill('20');
  await page.keyboard.press('Control+q');
  const dlg = page.getByRole('dialog', { name: 'Quit Wind?' });
  await expect(dlg.getByRole('button')).toHaveText(['Cancel', 'Discard', 'Save']);
  await dlg.getByRole('button', { name: 'Cancel' }).click();
  expect((await sent(page, 'window')).filter((m) => m.action === 'quitWind')).toHaveLength(0);
  await page.keyboard.press('Control+q');
  await page.getByRole('dialog').getByRole('button', { name: 'Discard' }).click();
  await expect.poll(async () => (await sent(page, 'window')).filter((m) => m.action === 'quitWind').length).toBe(1);
  expect(await sent(page, 'discardSession')).toHaveLength(1);
});

test('Quit Wind with nothing unsaved quits without a prompt', async ({ page }) => {
  await page.goto('/');
  await page.keyboard.press('Control+q');
  await expect(page.getByRole('dialog')).toHaveCount(0);
  expect((await sent(page, 'window')).filter((m) => m.action === 'quitWind')).toHaveLength(1);
});

test('switching profile with unsaved changes asks first; Cancel keeps everything', async ({ page }) => {
  await page.goto('/');
  await go(page, 'zoom');
  await maxLevel(page).fill('20');
  await go(page, 'prefs');
  await pickProfile(page, 'Gaming');
  const dlg = page.getByRole('dialog', { name: 'Unsaved changes' });
  await expect(dlg.getByRole('button')).toHaveText(['Cancel', 'Discard', 'Save']);
  await dlg.getByRole('button', { name: 'Cancel' }).click();
  expect(await sent(page, 'switchProfile')).toHaveLength(0);
  await expect(capsule(page)).toContainText('1 unsaved change');
});

test('profile switch prompt: Save saves then switches; Discard discards then switches', async ({ page }) => {
  await page.goto('/');
  await go(page, 'zoom');
  await maxLevel(page).fill('20');
  await go(page, 'prefs');
  await pickProfile(page, 'Gaming');
  await page.getByRole('dialog').getByRole('button', { name: 'Save' }).click();
  await expect.poll(async () => (await sent(page, 'switchProfile')).length).toBe(1);
  const order = await page.evaluate(() => window.__msgs.map((m) => m.type).filter((t) => t === 'saveSession' || t === 'switchProfile'));
  expect(order).toEqual(['saveSession', 'switchProfile']);
  expect(await page.evaluate(() => window.__saved.maxLevel)).toBe('20');
});

test('switching with nothing unsaved goes straight through', async ({ page }) => {
  await page.goto('/');
  await go(page, 'prefs');
  await pickProfile(page, 'Gaming');
  await expect(page.getByRole('dialog')).toHaveCount(0);
  expect(await sent(page, 'switchProfile')).toHaveLength(1);
});

test('engine change writes the ini, then Restart Wind relaunches; a failed relaunch reverts', async ({ page }) => {
  await page.goto('/');
  await go(page, 'prefs');
  await page.locator('[data-key="showAdvanced"]').getByRole('switch').check({ force: true });   // the engine row is advanced
  await go(page, 'zoom');
  await page.locator('[data-key="model"] select').selectOption('render');
  expect((await sent(page, 'setConfig')).some((m) => m.key === 'model' && m.value === 'render')).toBe(true);
  await page.getByRole('button', { name: 'Restart Wind' }).click();
  const types = await page.evaluate(() => window.__msgs.map((m) => m.type + ':' + (m.action || m.key || '')));
  expect(types.indexOf('setConfig:model')).toBeLessThan(types.indexOf('window:restartWind'));
  await page.evaluate(() => window.__hostSend({ type: 'restartFailed' }));
  await expect(page.getByRole('dialog', { name: "Couldn't restart Wind" })).toBeVisible();
  await page.getByRole('button', { name: 'Close' }).last().click();
  await expect(page.locator('[data-key="model"] select')).toHaveValue('hybrid');
  expect(await page.evaluate(() => window.__live.model)).toBe('hybrid');
});

test('High resolution cursor applies live: no MPO registry write, no restart prompt (#369)', async ({ page }) => {
  await page.goto('/');
  await go(page, 'view');
  const sw = page.locator('[data-key="txSamplingMode"]').getByRole('switch');
  await sw.check({ force: true });
  expect(await page.evaluate(() => window.__live.txSamplingMode)).toBe('1');
  await sw.click({ force: true });
  expect(await page.evaluate(() => window.__live.txSamplingMode)).toBe('0');
  expect(await sent(page, 'setMpoDisabled')).toHaveLength(0);
  await expect(page.getByRole('dialog', { name: 'Restart to finish' })).toHaveCount(0);
  await expect(page.locator('[data-key="txSamplingMode"]').getByText('Requires restart')).toHaveCount(0);
});

test('the tray switching profile reloads the session', async ({ page }) => {
  await page.goto('/');
  await go(page, 'zoom');
  await page.evaluate(() => {
    window.__live.maxLevel = '30'; window.__saved.maxLevel = '30'; window.__profiles.active = 'Gaming';
    window.__hostSend({ type: 'profiles', push: true, names: ['Default', 'Gaming'], active: 'Gaming', ok: true });
  });
  await expect(maxLevel(page)).toHaveValue('30');
  await expect(capsule(page)).toHaveCount(0);
});

test('regaining focus re-reads the ini, so a tray or hand edit shows up (and only a real change does)', async ({ page }) => {
  await page.goto('/');
  await go(page, 'zoom');
  await expect(maxLevel(page)).toHaveValue('12');
  const before = (await sent(page, 'getConfig')).length;
  await page.evaluate(() => { window.__live.maxLevel = '25'; window.dispatchEvent(new Event('focus')); });
  await expect(maxLevel(page)).toHaveValue('25');
  await expect(capsule(page)).toContainText('1 unsaved change');   // the tray edit is unsaved, like any live write
  expect((await sent(page, 'getConfig')).length).toBeGreaterThan(before);
});

test('a failed settings write re-reads the ini, so the page does not keep the value the ini refused', async ({ page }) => {
  await page.goto('/');
  await go(page, 'zoom');
  await maxLevel(page).fill('30');
  // The host kept the old value: undo the mock's write, then report the failure.
  await page.evaluate(() => { window.__live.maxLevel = '12'; window.__hostSend({ type: 'configWriteFailed', key: 'maxLevel' }); });
  await expect(maxLevel(page)).toHaveValue('12');
  await expect(capsule(page)).toHaveCount(0);
});

test('an unreadable ini is retried, never shown as all defaults', async ({ page }) => {
  await page.addInitScript(() => {
    const orig = window.chrome.webview.postMessage;
    let n = 0;
    window.chrome.webview.postMessage = (msg) => {
      if (msg.type === 'getConfig' && n++ < 2) { window.__msgs.push(msg); window.__hostSend({ type: 'configUnreadable' }); return; }
      orig(msg);
    };
  });
  await page.goto('/');
  await go(page, 'zoom');
  await expect(maxLevel(page)).toHaveValue('12');   // the real value, after two refused reads
  expect((await sent(page, 'getConfig')).length).toBe(3);
});
