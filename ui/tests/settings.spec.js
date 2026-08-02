import { test, expect } from '@playwright/test';

test.beforeEach(async ({ page }) => {
  await page.addInitScript(() => {
    window.__skipSplash = true;
    window.__sets = [];
    const listeners = new Set();
    window.chrome = { webview: {
      addEventListener: (_e, fn) => listeners.add(fn),
      postMessage: (msg) => {
        if (msg.type === 'getConfig')
          // showAdvanced=1: 'Cursor speed' and 'Smooth zoom' are advanced:true rows, so without it
          // they never render and the tests asserting on them time out looking for a hidden row.
          // model=render: 'Sharpness' additionally carries showIf {model:'render'}, and an unset
          // model fails that check (undefined !== 'render'), hiding the row the same way.
          listeners.forEach(fn => fn({ data: { type: 'config', values: { zoomInSpeed: '1.2', smoothZoom: '0', uiTheme: 'auto', showAdvanced: '1', model: 'render', zoomInButton: '2', zoomInVk: '33', zoomOutButton: '1', zoomOutVk: '34' } } }));
        if (msg.type === 'setConfig') window.__sets.push(msg);
        // MPO lives in the registry, not the ini. __mpoDisabled drives what the "registry" reports;
        // __mpoOk drives whether the elevated write is accepted (false = UAC dismissed).
        if (msg.type === 'mpoState')
          listeners.forEach(fn => fn({ data: { type: 'mpoState', disabled: !!window.__mpoDisabled } }));
        if (msg.type === 'setMpoDisabled') {
          const ok = window.__mpoOk !== false;
          if (ok) window.__mpoDisabled = msg.value === '1';
          listeners.forEach(fn => fn({ data: { type: 'mpoApplied', ok, disabled: !!window.__mpoDisabled } }));
        }
        if (msg.type === 'window') window.__sets.push(msg);
        // The host shows a native file picker and replies with the bare exe name. Stand in for it
        // with a settable name so a test can drive what the "picker" returns.
        if (msg.type === 'pickExe')
          listeners.forEach(fn => fn({ data: { type: 'exePicked', name: window.__pick || 'RDR2.exe' } }));
      },
    }};
  });
});

test('renders all sections on one page', async ({ page }) => {
  await page.goto('/');
  await expect(page.getByText('Zoom-in speed')).toBeVisible();
  await expect(page.getByText('Cursor speed')).toBeVisible();
  await expect(page.getByText('Sharpness')).toBeVisible();
  await expect(page.getByRole('heading', { name: 'About' })).toBeVisible();
});

test('rail click scrolls and marks the section active', async ({ page }) => {
  await page.goto('/');
  await page.getByRole('button', { name: 'Display' }).click();
  await expect(page.getByRole('button', { name: 'Display' })).toHaveClass(/active/);
});

test('theme toggle writes uiTheme', async ({ page }) => {
  await page.goto('/');
  await page.getByRole('button', { name: 'Toggle theme' }).click();
  const sets = await page.evaluate(() => window.__sets);
  expect(sets.some(s => s.key === 'uiTheme')).toBeTruthy();
});

test('changes stage until Apply, then setConfig fires', async ({ page }) => {
  await page.goto('/');
  await page.getByText('Smooth zoom', { exact: true }).locator('xpath=../..').getByRole('checkbox').click();
  expect(await page.evaluate(() => window.__sets.filter(s => s.key === 'smoothZoom').length)).toBe(0);
  await page.getByRole('button', { name: 'Apply' }).click();
  const sets = await page.evaluate(() => window.__sets);
  expect(sets.some(s => s.key === 'smoothZoom' && s.value === '1')).toBeTruthy();
});

test('a slot holding both a side-button and a key shows BOTH bindings', async ({ page }) => {
  await page.goto('/');
  // The mock binds zoom-in to Mouse button 5 AND PageUp (zoomInButton=2, zoomInVk=33). The core
  // OR-combines the two, so both really fire - the keycap must list both. Showing only the button
  // hid the key binding, which is how PageUp/PageDown kept zooming while Settings read
  // "Mouse button 5" and offered nothing to clear.
  const cap = page.getByText('Zoom in', { exact: true }).locator('xpath=../..').getByRole('button');
  await expect(cap).toHaveText(/Mouse button 5/);
  await expect(cap).toHaveText(/PageUp/);
});

// Issue #156: releasing keys trades swallowing for smooth panning, so it must be visible in the UI
// (an ini-only knob is one nobody finds) and must ship OFF - the mock config sets neither key.
// The row shows the current state plus one button; everything else lives in the dialog, so the
// row stays one line however many programs are listed.
test('the app row summarises an empty list and opens a dialog', async ({ page }) => {
  await page.goto('/');
  const row = page.getByText("Don't swallow keys in these apps", { exact: true }).locator('xpath=../..');
  await expect(row.getByText('None', { exact: true })).toBeVisible();
  await expect(page.getByRole('dialog')).toHaveCount(0);
  await row.getByRole('button', { name: 'Manage list' }).click();
  await expect(page.getByRole('dialog')).toBeVisible();
});

test('adding a program stages it until Apply, then setConfig fires', async ({ page }) => {
  await page.goto('/');
  await page.getByRole('button', { name: 'Manage list' }).click();
  await page.getByRole('button', { name: 'Add program...' }).click();   // mock picks RDR2.exe
  await expect(page.getByRole('dialog').getByText('RDR2.exe')).toBeVisible();
  expect(await page.evaluate(() => window.__sets.filter(s => s.key === 'noSwallowApps').length)).toBe(0);
  await page.getByRole('dialog').getByRole('button', { name: 'Close' }).click();
  await expect(page.getByRole('dialog')).toHaveCount(0);
  await page.getByRole('button', { name: 'Apply' }).click();
  const sets = await page.evaluate(() => window.__sets);
  expect(sets.some(s => s.key === 'noSwallowApps' && s.value === 'RDR2.exe')).toBeTruthy();
});

// The core matches exe names case-insensitively, so two spellings of one program would both take
// effect while the list looked broken. The add path has to reject the duplicate outright.
test('adding the same program twice is ignored, regardless of case', async ({ page }) => {
  await page.goto('/');
  await page.getByRole('button', { name: 'Manage list' }).click();
  const add = page.getByRole('button', { name: 'Add program...' });
  await add.click();
  await page.evaluate(() => { window.__pick = 'rdr2.EXE'; });   // same program, different case
  await add.click();
  await expect(page.getByRole('button', { name: /^Remove / })).toHaveCount(1);
});

test('removing a program empties the list again', async ({ page }) => {
  await page.goto('/');
  await page.getByRole('button', { name: 'Manage list' }).click();
  await page.getByRole('button', { name: 'Add program...' }).click();
  await page.getByRole('button', { name: 'Remove RDR2.exe' }).click();
  await expect(page.getByRole('dialog').getByText('No apps yet')).toBeVisible();
  await page.getByRole('dialog').getByRole('button', { name: 'Close' }).click();
  await expect(page.getByText('None', { exact: true })).toBeVisible();
});

// Closing is the only way out now that the confirm button is gone, so all three routes are load
// bearing: the X, Escape, and a backdrop click.
test('the backdrop closes the dialog', async ({ page }) => {
  await page.goto('/');
  await page.getByRole('button', { name: 'Manage list' }).click();
  await page.mouse.click(8, 8);   // outside the box, on the backdrop
  await expect(page.getByRole('dialog')).toHaveCount(0);
});

test('Escape closes the dialog', async ({ page }) => {
  await page.goto('/');
  await page.getByRole('button', { name: 'Manage list' }).click();
  await expect(page.getByRole('dialog')).toBeVisible();
  await page.keyboard.press('Escape');
  await expect(page.getByRole('dialog')).toHaveCount(0);
});

test('keybind capture writes a VK on keydown (live, no Apply needed)', async ({ page }) => {
  await page.goto('/');
  await page.getByText('Zoom in', { exact: true }).locator('xpath=../..').getByRole('button').click();
  await page.keyboard.press('F2'); // keyCode 113; fires both keydown and keyup
  // Keybind writes are live (KeybindCapture calls setConfig immediately so the magnifier core
  // hot-reloads the new key and the hook stops swallowing the previous binding). No Apply step.
  const sets = await page.evaluate(() => window.__sets);
  expect(sets.some(s => s.key === 'zoomInVk' && s.value === '113')).toBeTruthy();
});

// --- MPO row + unsaved-changes guard (issue #164) ---------------------------
// The MPO row reflects HKLM, not the ini, so it stages and applies on its own path. The TOGGLE is
// the detector - unticked means MPO is on - so these assert on its checked state rather than on any
// badge beside it. A badge saying the same thing was removed: it read as an action, not a state.
const mpoBox = page => page.getByText('Disable MPO').locator('xpath=../..').getByRole('checkbox');

test('MPO row shows unticked when MPO is still enabled', async ({ page }) => {
  await page.addInitScript(() => { window.__mpoDisabled = false; });
  await page.goto('/');
  await expect(page.getByText('Disable MPO')).toBeVisible();
  await expect(mpoBox(page)).not.toBeChecked();
  await expect(page.getByText('Requires restart')).toHaveCount(0);
});

test('MPO row shows ticked once MPO is disabled', async ({ page }) => {
  await page.addInitScript(() => { window.__mpoDisabled = true; });
  await page.goto('/');
  await expect(mpoBox(page)).toBeChecked();
  await expect(page.getByText('Requires restart')).toHaveCount(0);
});

test('staging MPO shows "Requires restart" and prompts to reboot on Apply', async ({ page }) => {
  await page.addInitScript(() => { window.__mpoDisabled = false; });
  await page.goto('/');
  await mpoBox(page).check();
  await expect(page.getByText('Requires restart')).toBeVisible();
  await page.getByRole('button', { name: 'Apply' }).click();
  await expect(page.getByRole('dialog')).toContainText('Restart to finish');
  // Cancel must leave the registry change in place - only the reboot is deferred - so the toggle
  // stays ticked and nothing is left staged.
  await page.getByRole('dialog').getByRole('button', { name: 'Cancel' }).click();
  await expect(mpoBox(page)).toBeChecked();
  await expect(page.getByText('Requires restart')).toHaveCount(0);
});

test('a dismissed admin prompt reverts the MPO toggle', async ({ page }) => {
  await page.addInitScript(() => { window.__mpoDisabled = false; window.__mpoOk = false; });
  await page.goto('/');
  await mpoBox(page).check();
  await page.getByRole('button', { name: 'Apply' }).click();
  await expect(page.getByRole('dialog')).toContainText('MPO change not applied');
  // Scoped to the dialog: the title bar also has a button named Close.
  await page.getByRole('dialog').getByRole('button', { name: 'Close' }).click();
  // Reverted: back to unticked, with nothing left staged.
  await expect(mpoBox(page)).not.toBeChecked();
  await expect(page.getByText('Requires restart')).toHaveCount(0);
});

test('closing with unsaved changes asks before discarding', async ({ page }) => {
  await page.goto('/');
  // The title-bar X, not the footer Discard: both a footer button and the dialog are named
  // "Discard", so every button here is located precisely.
  const titleClose = page.locator('button.tbtn.close');
  await page.getByText('Smooth zoom', { exact: true }).locator('xpath=../..').getByRole('checkbox').click();
  await titleClose.click();
  await expect(page.getByRole('dialog')).toContainText('Settings not applied');
  // Cancel keeps the window and the staged change.
  await page.getByRole('dialog').getByRole('button', { name: 'Cancel' }).click();
  await expect(page.getByRole('dialog')).toHaveCount(0);
  let sets = await page.evaluate(() => window.__sets);
  expect(sets.some(s => s.type === 'window' && s.action === 'close')).toBeFalsy();
  // Discard closes for real, with force so the host guard does not re-ask.
  await titleClose.click();
  await page.getByRole('dialog').getByRole('button', { name: 'Discard' }).click();
  sets = await page.evaluate(() => window.__sets);
  expect(sets.some(s => s.type === 'window' && s.action === 'close' && s.force === '1')).toBeTruthy();
});

test('closing with nothing staged does not prompt', async ({ page }) => {
  await page.goto('/');
  await page.locator('button.tbtn.close').click();
  await expect(page.getByRole('dialog')).toHaveCount(0);
  const sets = await page.evaluate(() => window.__sets);
  expect(sets.some(s => s.type === 'window' && s.action === 'close')).toBeTruthy();
});
