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
          listeners.forEach(fn => fn({ data: { type: 'config', values: { zoomInSpeed: '1.2', smoothZoom: '0', uiTheme: 'auto', showAdvanced: '1', model: 'render', zoomInButton: '2', zoomInVk: '33', zoomOutButton: '1', zoomOutVk: '34', cursorLockVk: '113' } } }));
        if (msg.type === 'setConfig') window.__sets.push(msg);
        // MPO lives in the registry, not the ini. __mpoDisabled drives what the "registry" reports;
        // __mpoOk drives whether the elevated write is accepted (false = UAC dismissed).
        if (msg.type === 'mpoState')
          // __mpoAtBoot defaults to the current value, i.e. "the registry is what DWM loaded".
          // Set it separately to model a registry that has already moved away from the boot state.
          listeners.forEach(fn => fn({ data: { type: 'mpoState',
            disabled: !!window.__mpoDisabled,
            bootKnown: window.__mpoBootKnown !== false,
            atBoot: window.__mpoAtBoot !== undefined ? !!window.__mpoAtBoot : !!window.__mpoDisabled } }));
        if (msg.type === 'setMpoDisabled') {
          const ok = window.__mpoOk !== false;
          if (ok) window.__mpoDisabled = msg.value === '1';
          listeners.forEach(fn => fn({ data: { type: 'mpoApplied', ok, disabled: !!window.__mpoDisabled } }));
        }
        if (msg.type === 'window') {
          window.__sets.push(msg);
          // __restartFail: the host failed to relaunch Wind after a model change.
          if (msg.action === 'restartWind' && window.__restartFail)
            listeners.forEach(fn => fn({ data: { type: 'restartFailed' } }));
        }
        // The host shows a native file picker and replies with the bare exe name. Stand in for it
        // with a settable name so a test can drive what the "picker" returns.
        if (msg.type === 'pickExe')
          listeners.forEach(fn => fn({ data: { type: 'exePicked', name: window.__pick || 'RDR2.exe' } }));
        // Profiles: an in-page stand-in for the host's file ops, same reply shape as the C++ host.
        if (String(msg.type || '').match(/Profile$|^listProfiles$/)) window.__sets.push(msg);
        window.__profiles = window.__profiles || { names: ['Default', 'Gaming'], active: 'Default' };
        const reply = (ok = true, error = '') => listeners.forEach(fn => fn({ data: {
          type: 'profiles', names: [...window.__profiles.names],
          active: window.__profiles.active, ok, error } }));
        // __profileFail = '<messageType>' forces that operation to reply ok=false.
        if (window.__profileFail && msg.type === window.__profileFail) { reply(false, 'Simulated failure'); return; }
        if (msg.type === 'listProfiles') reply();
        if (msg.type === 'switchProfile') { window.__profiles.active = msg.name; reply(); }
        if (msg.type === 'createProfile') {
          window.__profiles.names.push(msg.name); window.__profiles.active = msg.name; reply();
        }
        if (msg.type === 'renameProfile') {
          window.__profiles.names = window.__profiles.names.map(n => n === msg.from ? msg.to : n);
          if (window.__profiles.active === msg.from) window.__profiles.active = msg.to;
          reply();
        }
        if (msg.type === 'duplicateProfile') { window.__profiles.names.push(msg.name + ' copy'); reply(); }
        if (msg.type === 'deleteProfile') {
          window.__profiles.names = window.__profiles.names.filter(n => n !== msg.name);
          if (window.__profiles.active === msg.name) window.__profiles.active = window.__profiles.names[0];
          reply();
        }
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
  // Cancel leaves the registry change in place and only defers the reboot, so the toggle stays
  // ticked AND the chip stays up: the value is written but DWM is still running the old one.
  await page.getByRole('dialog').getByRole('button', { name: 'Cancel' }).click();
  await expect(mpoBox(page)).toBeChecked();
  await expect(page.getByText('Requires restart')).toBeVisible();
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

// The bug this pair guards (issue #164, caught in use): DWM reads OverlayTestMode once, at boot.
// Comparing the staged value against the REGISTRY made restoring the boot value demand a pointless
// reboot. The comparison is against the BOOT state, so restoring it is a no-op and a real change
// still prompts.
test('putting MPO back to the boot state needs no restart', async ({ page }) => {
  // DWM booted with MPO disabled; the registry has since been changed to enabled.
  await page.addInitScript(() => { window.__mpoDisabled = false; window.__mpoAtBoot = true; });
  await page.goto('/');
  // Nothing staged yet, but the registry already disagrees with what is running - say so.
  await expect(page.getByText('Requires restart')).toBeVisible();
  await mpoBox(page).check();                       // back to the boot state
  await expect(page.getByText('Requires restart')).toHaveCount(0);
  await page.getByRole('button', { name: 'Apply' }).click();
  await expect(page.getByRole('dialog')).toHaveCount(0);   // written, but nothing to reboot for
  await expect(mpoBox(page)).toBeChecked();
});

test('moving MPO away from the boot state still prompts to restart', async ({ page }) => {
  await page.addInitScript(() => { window.__mpoDisabled = true; window.__mpoAtBoot = true; });
  await page.goto('/');
  await expect(page.getByText('Requires restart')).toHaveCount(0);
  await mpoBox(page).uncheck();
  await expect(page.getByText('Requires restart')).toBeVisible();
  await page.getByRole('button', { name: 'Apply' }).click();
  await expect(page.getByRole('dialog')).toContainText('Restart to finish');
});

// --- Profiles (spec 2026-08-12): titlebar dropdown ---------------------------
test('titlebar shows the active profile and lists all profiles on click', async ({ page }) => {
  await page.goto('/');
  const trigger = page.getByRole('button', { name: /Default/ });
  await expect(trigger).toBeVisible();
  await trigger.click();
  await expect(page.getByRole('menuitemradio', { name: /Gaming/ })).toBeVisible();
  await expect(page.getByRole('menuitem', { name: /Create new profile/ })).toBeVisible();
});

test('clicking another profile switches and reloads', async ({ page }) => {
  await page.goto('/');
  await page.getByRole('button', { name: /Default/ }).click();
  await page.getByRole('menuitemradio', { name: /Gaming/ }).click();
  await expect(page.getByRole('button', { name: /Gaming/ })).toBeVisible();
  const sets = await page.evaluate(() => window.__sets);
  expect(sets.some(s => s.type === 'switchProfile' && s.name === 'Gaming')).toBeTruthy();
});

test('create validates the name inline and sends createProfile when valid', async ({ page }) => {
  await page.goto('/');
  await page.getByRole('button', { name: /Default/ }).click();
  await page.getByRole('menuitem', { name: /Create new profile/ }).click();
  const input = page.getByPlaceholder('New profile name');
  await input.fill('Gaming');                       // duplicate
  await page.getByRole('button', { name: 'Create', exact: true }).click();
  await expect(page.getByText(/already exists/)).toBeVisible();
  await input.fill('Movies');
  await page.getByRole('button', { name: 'Create', exact: true }).click();
  const sets = await page.evaluate(() => window.__sets);
  expect(sets.some(s => s.type === 'createProfile' && s.name === 'Movies')).toBeTruthy();
});

test('right-click opens rename/duplicate/delete; rename round-trips', async ({ page }) => {
  await page.goto('/');
  await page.getByRole('button', { name: /Default/ }).click();
  await page.getByRole('menuitemradio', { name: /Gaming/ }).click({ button: 'right' });
  await page.getByRole('menuitem', { name: 'Rename' }).click();
  await page.getByPlaceholder('New name').fill('Games');
  await page.getByRole('button', { name: 'Rename', exact: true }).click();
  const sets = await page.evaluate(() => window.__sets);
  expect(sets.some(s => s.type === 'renameProfile' && s.from === 'Gaming' && s.to === 'Games')).toBeTruthy();
});

test('delete is disabled on the last profile', async ({ page }) => {
  await page.addInitScript(() => { window.__profiles = { names: ['Solo'], active: 'Solo' }; });
  await page.goto('/');
  await page.getByRole('button', { name: /Solo/ }).click();
  await page.getByRole('menuitemradio', { name: /Solo/ }).click({ button: 'right' });
  await expect(page.getByRole('menuitem', { name: 'Delete' })).toBeDisabled();
});

test('switching with staged changes raises the unsaved-changes guard', async ({ page }) => {
  await page.goto('/');
  await page.getByText('Smooth zoom', { exact: true }).locator('xpath=../..').getByRole('checkbox').click();
  await page.getByRole('button', { name: /Default/ }).click();
  await page.getByRole('menuitemradio', { name: /Gaming/ }).click();
  await expect(page.getByText('Unsaved changes')).toBeVisible();
  await page.getByRole('button', { name: 'Discard and continue' }).click();
  await expect(page.getByRole('button', { name: /Gaming/ })).toBeVisible();
});

test('a failed profile action surfaces a visible error dialog', async ({ page }) => {
  await page.addInitScript(() => { window.__profileFail = 'switchProfile'; });
  await page.goto('/');
  await page.getByRole('button', { name: /Default/ }).click();
  await page.getByRole('menuitemradio', { name: /Gaming/ }).click();
  await expect(page.getByText('Profile action failed')).toBeVisible();
  await expect(page.getByText('Simulated failure')).toBeVisible();
  await page.getByRole('dialog').getByRole('button', { name: 'Close' }).click();
  await expect(page.getByText('Profile action failed')).toHaveCount(0);
});

test('deleting a NON-active profile with staged changes skips the guard', async ({ page }) => {
  await page.goto('/');
  await page.getByText('Smooth zoom', { exact: true }).locator('xpath=../..').getByRole('checkbox').click();
  await page.getByRole('button', { name: /Default/ }).click();
  await page.getByRole('menuitemradio', { name: /Gaming/ }).click({ button: 'right' });
  await page.getByRole('menuitem', { name: 'Delete' }).click();          // one-click delete
  await expect(page.getByText('Unsaved changes')).toHaveCount(0);      // no guard
  const sets = await page.evaluate(() => window.__sets);
  expect(sets.some(s => s.type === 'deleteProfile' && s.name === 'Gaming')).toBeTruthy();
});

test('a leading-dot name is rejected inline before reaching the host', async ({ page }) => {
  await page.goto('/');
  await page.getByRole('button', { name: /Default/ }).click();
  await page.getByRole('menuitem', { name: /Create new profile/ }).click();
  await page.getByPlaceholder('New profile name').fill('.hidden');
  await page.getByRole('button', { name: 'Create', exact: true }).click();
  await expect(page.getByText(/space or dot/)).toBeVisible();
  expect(await page.evaluate(() => window.__sets.filter(s => s.type === 'createProfile').length)).toBe(0);
});

test('Escape closes the profile dropdown', async ({ page }) => {
  await page.goto('/');
  await page.getByRole('button', { name: /Default/ }).click();
  await expect(page.getByRole('menuitemradio', { name: /Gaming/ })).toBeVisible();
  await page.keyboard.press('Escape');
  await expect(page.getByRole('menuitemradio', { name: /Gaming/ })).toHaveCount(0);
});

test('the Default profile cannot be deleted even among many', async ({ page }) => {
  await page.goto('/');
  await page.getByRole('button', { name: /Default/ }).click();
  await page.getByRole('menuitem', { name: 'Profile actions for Default' }).click();
  await expect(page.getByRole('menuitem', { name: 'Delete' })).toBeDisabled();
});

// --- Review fixes (issue #182) ----------------------------------------------
test('the Inspect row shows its real binding (cursorLockVk is loaded)', async ({ page }) => {
  await page.goto('/');
  // Mock binds cursorLockVk=113 (F2). The row lied ("Unbound") before the fix because
  // vkKey-only rows were never loaded into values.
  const cap = page.getByText('Inspect mode', { exact: true }).locator('xpath=../..').getByRole('button');
  await expect(cap).toHaveText(/F2/);
});

test('forbidden keys are refused during keybind capture and capture stays armed', async ({ page }) => {
  await page.goto('/');
  await page.getByText('Zoom in', { exact: true }).locator('xpath=../..').getByRole('button').click();
  await page.keyboard.press('Backspace');   // forbidden (would be swallowed system-wide)
  // Arming live-clears the previous binding (one zoomInVk=0 write is expected); the forbidden
  // key itself must never be written.
  expect(await page.evaluate(() =>
    window.__sets.filter(s => s.key === 'zoomInVk' && s.value !== '0').length)).toBe(0);
  await page.keyboard.press('F2');          // capture must still be armed
  const sets = await page.evaluate(() => window.__sets);
  expect(sets.some(s => s.key === 'zoomInVk' && s.value === '113')).toBeTruthy();
});

test('a model change writes the ini BEFORE requesting the relaunch', async ({ page }) => {
  await page.goto('/');
  const row = page.getByText('Magnifier model', { exact: true }).locator('xpath=../..');
  await row.getByRole('combobox').click();
  await page.getByRole('option', { name: 'Windows Magnifier' }).click();
  await page.getByRole('button', { name: 'Apply' }).click();
  const sets = await page.evaluate(() => window.__sets);
  const iModel = sets.findIndex(s => s.key === 'model' && s.value === 'magnify');
  const iRestart = sets.findIndex(s => s.type === 'window' && s.action === 'restartWind');
  expect(iModel).toBeGreaterThanOrEqual(0);
  expect(iRestart).toBeGreaterThan(iModel);   // the relaunched Wind reads the ini at startup
});

test('a failed relaunch reverts the model dropdown and the ini', async ({ page }) => {
  await page.addInitScript(() => { window.__restartFail = true; });
  await page.goto('/');
  const row = page.getByText('Magnifier model', { exact: true }).locator('xpath=../..');
  await row.getByRole('combobox').click();
  await page.getByRole('option', { name: 'Windows Magnifier' }).click();
  await page.getByRole('button', { name: 'Apply' }).click();
  await expect(page.getByText("Couldn't restart Wind")).toBeVisible();
  const sets = await page.evaluate(() => window.__sets);
  // The revert write puts the RUNNING model back after the failed switch attempt.
  const last = sets.filter(s => s.key === 'model').pop();
  expect(last.value).toBe('render');
});

test('desktopTransform row shows only for the Auto model (advanced)', async ({ page }) => {
  await page.goto('/');
  // Mock config has model=render + showAdvanced=1: the row must be hidden.
  await expect(page.getByText('Game engine on the desktop')).toHaveCount(0);
  const row = page.getByText('Magnifier model', { exact: true }).locator('xpath=../..');
  await row.getByRole('combobox').click();
  await page.getByRole('option', { name: 'Auto' }).click();
  await expect(page.getByText('Game engine on the desktop')).toBeVisible();
});
