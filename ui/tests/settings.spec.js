import { test, expect } from '@playwright/test';

test.beforeEach(async ({ page }) => {
  await page.addInitScript(() => {
    window.__skipSplash = true;
    window.__sets = [];
    const listeners = new Set();
    // Lets a test play the host: deliver a message exactly as WebView2 would.
    window.__hostSend = (data) => listeners.forEach(fn => fn({ data }));
    window.chrome = { webview: {
      addEventListener: (_e, fn) => listeners.add(fn),
      postMessage: (msg) => {
        if (msg.type === 'getConfig')
          // showAdvanced=1: 'Cursor speed' and 'Smooth zoom' are advanced:true rows, so without it
          // they never render and the tests asserting on them time out looking for a hidden row.
          // model=render: 'Sharpness' additionally carries showIf {model:'render'}, and an unset
          // model fails that check (undefined !== 'render'), hiding the row the same way.
          // __cfgSampling seeds txSamplingMode (the combined high-res/MPO option, issue #242).
          listeners.forEach(fn => fn({ data: { type: 'config', values: { zoomInSpeed: '1.2', smoothZoom: '0', uiTheme: 'auto', showAdvanced: '1', model: 'render', zoomInButton: '2', zoomInVk: '33', zoomOutButton: '1', zoomOutVk: '34', cursorLockVk: '113', txSamplingMode: window.__cfgSampling !== undefined ? window.__cfgSampling : '0' } } }));
        if (msg.type === 'setConfig') window.__sets.push(msg);
        if (msg.type === 'openRepo') window.__sets.push(msg);
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
  await expect(page.getByText('Max zoom')).toBeVisible();
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
  // Staged via the high-resolution-cursor toggle (Cursor section; the alternate-keybinds
  // gate left the UI 2026-08-22 - both keybind slots are always visible now).
  await page.getByRole('button', { name: 'Cursor', exact: true }).click();
  await page.getByText('High resolution cursor', { exact: true })
      .locator('xpath=../..').getByRole('checkbox').click();
  expect(await page.evaluate(() => window.__sets.filter(s => s.key === 'txSamplingMode').length)).toBe(0);
  await page.getByRole('button', { name: 'Apply' }).click();
  const sets = await page.evaluate(() => window.__sets);
  expect(sets.some(s => s.key === 'txSamplingMode' && s.value === '1')).toBeTruthy();
});

test('each zoom direction is ONE row with TWO capture slots', async ({ page }) => {
  await page.goto('/');
  await expect(page.getByText('Zoom in',  { exact: true }).locator('xpath=../..').getByRole('button')).toHaveCount(2);
  await expect(page.getByText('Zoom out', { exact: true }).locator('xpath=../..').getByRole('button')).toHaveCount(2);
});

test('a slot holding both a side-button and a key shows BOTH bindings', async ({ page }) => {
  await page.goto('/');
  // The mock binds zoom-in to Mouse button 5 AND PageUp (zoomInButton=2, zoomInVk=33). The core
  // OR-combines the two, so both really fire - the keycap must list both. Showing only the button
  // hid the key binding, which is how PageUp/PageDown kept zooming while Settings read
  // "Mouse button 5" and offered nothing to clear.
  const cap = page.getByText('Zoom in', { exact: true }).locator('xpath=../..').getByRole('button').first();
  await expect(cap).toHaveText(/Mouse button 5/);
  await expect(cap).toHaveText(/PageUp/);
});

// Issue #156: releasing keys trades swallowing for smooth panning, so it must be visible in the UI
// (an ini-only knob is one nobody finds) and must ship OFF - the mock config sets neither key.
// The row shows the current state plus one button; everything else lives in the dialog, so the
// row stays one line however many programs are listed.
test('the app row summarises an empty list and opens a dialog', async ({ page }) => {
  await page.goto('/');
  const row = page.getByText("Pass zoom keys to these apps", { exact: true }).locator('xpath=../..');
  await expect(row.getByText('None', { exact: true })).toBeVisible();
  await expect(page.getByRole('dialog')).toHaveCount(0);
  await row.getByRole('button', { name: 'Manage list' }).click();
  await expect(page.getByRole('dialog')).toBeVisible();
});

test('adding a program stages it until Apply, then setConfig fires', async ({ page }) => {
  await page.goto('/');
  await page.getByRole('button', { name: 'Manage list' }).first().click();   // two applists exist now (lockApps, #221): target the first (noSwallowApps)
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
  await page.getByRole('button', { name: 'Manage list' }).first().click();   // two applists exist now (lockApps, #221): target the first (noSwallowApps)
  const add = page.getByRole('button', { name: 'Add program...' });
  await add.click();
  await page.evaluate(() => { window.__pick = 'rdr2.EXE'; });   // same program, different case
  await add.click();
  await expect(page.getByRole('button', { name: /^Remove / })).toHaveCount(1);
});

test('removing a program empties the list again', async ({ page }) => {
  await page.goto('/');
  await page.getByRole('button', { name: 'Manage list' }).first().click();   // two applists exist now (lockApps, #221): target the first (noSwallowApps)
  await page.getByRole('button', { name: 'Add program...' }).click();
  await page.getByRole('button', { name: 'Remove RDR2.exe' }).click();
  await expect(page.getByRole('dialog').getByText('No apps yet')).toBeVisible();
  await page.getByRole('dialog').getByRole('button', { name: 'Close' }).click();
  await expect(page.getByText('None', { exact: true }).first()).toBeVisible();   // both applists show 'None' when empty (#221)
});

// Closing is the only way out now that the confirm button is gone, so all three routes are load
// bearing: the X, Escape, and a backdrop click.
test('the backdrop closes the dialog', async ({ page }) => {
  await page.goto('/');
  await page.getByRole('button', { name: 'Manage list' }).first().click();   // two applists exist now (lockApps, #221): target the first (noSwallowApps)
  await page.mouse.click(8, 8);   // outside the box, on the backdrop
  await expect(page.getByRole('dialog')).toHaveCount(0);
});

test('Escape closes the dialog', async ({ page }) => {
  await page.goto('/');
  await page.getByRole('button', { name: 'Manage list' }).first().click();   // two applists exist now (lockApps, #221): target the first (noSwallowApps)
  await expect(page.getByRole('dialog')).toBeVisible();
  await page.keyboard.press('Escape');
  await expect(page.getByRole('dialog')).toHaveCount(0);
});

test('keybind capture writes a VK on keydown (live, no Apply needed)', async ({ page }) => {
  await page.goto('/');
  await page.getByText('Zoom in', { exact: true }).locator('xpath=../..').getByRole('button').first().click();
  await page.keyboard.press('F2'); // keyCode 113; fires both keydown and keyup
  // Keybind writes are live (KeybindCapture calls setConfig immediately so the magnifier core
  // hot-reloads the new key and the hook stops swallowing the previous binding). No Apply step.
  const sets = await page.evaluate(() => window.__sets);
  expect(sets.some(s => s.key === 'zoomInVk' && s.value === '113')).toBeTruthy();
});

// --- High-res + MPO combined option (issue #242; MPO staging from issue #164) ----------------
// The one toggle drives BOTH halves: its checked state is the INI value (txSamplingMode), and
// editing it mirrors the staged MPO half (crisp stages MPO-disable, high-res stages MPO-enable),
// because crisp magnification with MPO enabled is the driver-crash combo. The registry half still
// applies through the staged UAC + restart flow.
const hiResBox = page => page.getByText('High resolution cursor', { exact: true })
    .locator('xpath=../..').getByRole('checkbox');

test('the combined toggle reflects the ini value, not the registry', async ({ page }) => {
  // MPO already disabled, but high-res off in the ini: the toggle must show the INI half.
  await page.addInitScript(() => { window.__mpoDisabled = true; });
  await page.goto('/');
  await expect(hiResBox(page)).not.toBeChecked();
  await expect(page.getByText('Requires restart')).toHaveCount(0);
});

test('turning high-res OFF stages MPO-disable and prompts to restart on Apply', async ({ page }) => {
  await page.addInitScript(() => { window.__mpoDisabled = false; window.__cfgSampling = '1'; });
  await page.goto('/');
  await hiResBox(page).uncheck();
  await expect(page.getByText('Requires restart')).toBeVisible();
  await page.getByRole('button', { name: 'Apply' }).click();
  await expect(page.getByRole('dialog')).toContainText('Restart to finish');
  // Cancel leaves the registry change in place and only defers the reboot: the chip stays up
  // (the value is written but DWM is still running the old one), and the ini half landed too.
  await page.getByRole('dialog').getByRole('button', { name: 'Cancel' }).click();
  await expect(page.getByText('Requires restart')).toBeVisible();
  const sets = await page.evaluate(() => window.__sets);
  expect(sets.some(s => s.key === 'txSamplingMode' && s.value === '0')).toBeTruthy();
});

test('turning high-res ON stages MPO re-enable', async ({ page }) => {
  await page.addInitScript(() => { window.__mpoDisabled = true; window.__cfgSampling = '0'; });
  await page.goto('/');
  await hiResBox(page).check();
  await expect(page.getByText('Requires restart')).toBeVisible();
  await page.getByRole('button', { name: 'Apply' }).click();
  await expect(page.getByRole('dialog')).toContainText('Restart to finish');
  const sets = await page.evaluate(() => window.__sets);
  expect(sets.some(s => s.key === 'txSamplingMode' && s.value === '1')).toBeTruthy();
});

test('a dismissed admin prompt reverts BOTH halves of the option', async ({ page }) => {
  await page.addInitScript(() => {
    window.__mpoDisabled = false; window.__mpoOk = false; window.__cfgSampling = '1';
  });
  await page.goto('/');
  await hiResBox(page).uncheck();
  await page.getByRole('button', { name: 'Apply' }).click();
  await expect(page.getByRole('dialog')).toContainText('MPO change not applied');
  // Scoped to the dialog: the title bar also has a button named Close.
  await page.getByRole('dialog').getByRole('button', { name: 'Close' }).click();
  // Reverted wholesale: the toggle is back ON, nothing staged, and the ini half never landed -
  // a half-applied "crisp" would be exactly the crisp+MPO-on combo the coupling prevents.
  await expect(hiResBox(page)).toBeChecked();
  await expect(page.getByText('Requires restart')).toHaveCount(0);
  expect(await page.evaluate(() =>
    window.__sets.filter(s => s.key === 'txSamplingMode' && s.value === '0').length)).toBe(0);
});

test('closing with unsaved changes asks before discarding', async ({ page }) => {
  await page.goto('/');
  // The title-bar X, not the footer Discard: both a footer button and the dialog are named
  // "Discard", so every button here is located precisely.
  const titleClose = page.locator('button.tbtn.close');
  await page.getByRole('button', { name: 'Cursor', exact: true }).click();
  await page.getByText('High resolution cursor', { exact: true }).locator('xpath=../..').getByRole('checkbox').click();
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
  // DWM booted with MPO disabled; the registry has since been changed to enabled (the chip must
  // say so on load, with nothing staged). Toggling high-res ON then OFF stages MPO-disable while
  // leaving the ini value where it started - back to the boot state, so the chip clears and
  // Apply writes the registry without demanding a pointless reboot.
  await page.addInitScript(() => { window.__mpoDisabled = false; window.__mpoAtBoot = true; window.__cfgSampling = '0'; });
  await page.goto('/');
  await expect(page.getByText('Requires restart')).toBeVisible();
  await hiResBox(page).check();
  await hiResBox(page).uncheck();                   // stages MPO-disable = the boot state
  await expect(page.getByText('Requires restart')).toHaveCount(0);
  await page.getByRole('button', { name: 'Apply' }).click();
  await expect(page.getByRole('dialog')).toHaveCount(0);   // written, but nothing to reboot for
});

test('moving MPO away from the boot state still prompts to restart', async ({ page }) => {
  await page.addInitScript(() => { window.__mpoDisabled = true; window.__mpoAtBoot = true; window.__cfgSampling = '0'; });
  await page.goto('/');
  await expect(page.getByText('Requires restart')).toHaveCount(0);
  await hiResBox(page).check();                     // high-res ON stages MPO re-enable
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
  await page.getByRole('button', { name: 'Cursor', exact: true }).click();
  await page.getByText('High resolution cursor', { exact: true }).locator('xpath=../..').getByRole('checkbox').click();
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
  await page.getByRole('button', { name: 'Cursor', exact: true }).click();
  await page.getByText('High resolution cursor', { exact: true }).locator('xpath=../..').getByRole('checkbox').click();
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
  await page.getByText('Zoom in', { exact: true }).locator('xpath=../..').getByRole('button').first().click();
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
  const row = page.getByText('Magnifier engine', { exact: true }).locator('xpath=../..');
  await row.getByRole('combobox').click();
  await page.getByRole('option', { name: 'System' }).click();
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
  const row = page.getByText('Magnifier engine', { exact: true }).locator('xpath=../..');
  await row.getByRole('combobox').click();
  await page.getByRole('option', { name: 'System' }).click();
  await page.getByRole('button', { name: 'Apply' }).click();
  await expect(page.getByText("Couldn't restart Wind")).toBeVisible();
  const sets = await page.evaluate(() => window.__sets);
  // The revert write puts the RUNNING model back after the failed switch attempt.
  const last = sets.filter(s => s.key === 'model').pop();
  expect(last.value).toBe('render');
});

// (The desktopTransform showIf test left with its row in the 2026-08-21 cleanup - the knob is
// ini-only now. Restore from git history if the row returns.)

test('About: Star on GitHub asks the host to open the repo, and shows the real version', async ({ page }) => {
  await page.goto('/');
  const star = page.getByRole('button', { name: 'Star on GitHub' });
  await star.scrollIntoViewIfNeeded();
  await star.click();
  const sent = await page.evaluate(() => window.__sets.filter(m => m.type === 'openRepo'));
  expect(sent).toEqual([{ type: 'openRepo' }]);
  // not a link any more: a followed link would open a WebView popup, not the user's browser
  await expect(page.locator('.about-hero a')).toHaveCount(0);
  await expect(page.locator('.about-hero .version')).toHaveText(/^v\d+\.\d+\.\d+/);
});

test('a settings write the host could not save shows a dialog (issue #274)', async ({ page }) => {
  await page.goto('/');
  await expect(page.getByText('Keybinds').first()).toBeVisible();
  await page.evaluate(() => window.__hostSend({ type: 'configWriteFailed', key: 'zoomInSpeed' }));
  const dlg = page.getByRole('dialog', { name: "Couldn't save the setting" });
  await expect(dlg).toBeVisible();
  await expect(dlg).toContainText('zoomInSpeed');
  await dlg.getByRole('button', { name: 'Close' }).click();
  await expect(dlg).toHaveCount(0);
});

// Tracking modes (issue #276): caret tracking defaults ON, focus tracking OFF, alignment
// centred by default. Toggle rows expose role="checkbox" here (see Row.svelte), not "switch",
// so the query matches the rest of this file rather than the plan's draft.
test('Tracking section: caret on, focus off, centred by default (issue #276)', async ({ page }) => {
  await page.goto('/');
  const caret = page.getByText('Follow the text cursor', { exact: true }).locator('xpath=../..').getByRole('checkbox');
  const focus = page.getByText('Follow keyboard focus', { exact: true }).locator('xpath=../..').getByRole('checkbox');
  await expect(caret).toBeChecked();
  await expect(focus).not.toBeChecked();
  await focus.click();
  await page.getByRole('button', { name: 'Apply' }).click();
  const sets = await page.evaluate(() => window.__sets.filter(m => m.type === 'setConfig' && m.key === 'trackFocus'));
  expect(sets.at(-1).value).toBe('1');
});
