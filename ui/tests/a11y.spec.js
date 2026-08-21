// Screen-reader / keyboard regression tests (issue #201).
//
// A coworker reported the settings window was cumbersome to navigate with a screen reader. The
// cause was not exotic: nothing in the settings list had an accessible NAME. The label and the
// description are sibling <div>s of the control, so the accessibility tree read "checkbox,
// checked" and "slider, 1.2" with no indication of which setting had been reached, for all ~24
// controls. These tests pin that fix and the keyboard/focus work that went with it, because every
// one of them is invisible in a screenshot and none of the existing 38 tests could catch a
// regression.
import { test, expect } from '@playwright/test';

test.beforeEach(async ({ page }) => {
  await page.addInitScript(() => {
    window.__skipSplash = true;
    window.__sets = [];
    const listeners = new Set();
    window.chrome = { webview: {
      addEventListener: (_e, fn) => listeners.add(fn),
      postMessage: (msg) => {
        // showAdvanced=1 + model=render so the advanced and render-only rows are all present:
        // this suite asserts over EVERY rendered control, so the more of them the better.
        if (msg.type === 'getConfig')
          listeners.forEach(fn => fn({ data: { type: 'config', values: {
            zoomInSpeed: '1.2', smoothZoom: '0', uiTheme: 'auto', showAdvanced: '1',
            model: 'render', zoomInButton: '2', zoomInVk: '33', zoomOutButton: '1',
            zoomOutVk: '34', cursorLockVk: '113', outline: '1', onboarded: '1' } } }));
        if (msg.type === 'setConfig') window.__sets.push(msg);
        if (msg.type === 'mpoState')
          listeners.forEach(fn => fn({ data: { type: 'mpoState',
            disabled: false, bootKnown: true, atBoot: false } }));
        if (msg.type === 'pickExe')
          listeners.forEach(fn => fn({ data: { type: 'exePicked', name: 'RDR2.exe' } }));
        window.__profiles = window.__profiles || { names: ['Default', 'Gaming'], active: 'Default' };
        const reply = () => listeners.forEach(fn => fn({ data: { type: 'profiles',
          names: [...window.__profiles.names], active: window.__profiles.active, ok: true } }));
        if (String(msg.type || '').match(/Profile$|^listProfiles$/)) { window.__sets.push(msg); reply(); }
      },
    }};
  });
  await page.goto('/');
  await page.getByText('Zoom-in speed').waitFor();
});

// --- Naming: the finding that started this ---------------------------------

test('every control in the settings list has an accessible name', async ({ page }) => {
  // The original failure, asserted directly against the accessibility tree rather than the DOM:
  // a control whose name is empty is one a screen reader announces as a bare role.
  const unnamed = await page.locator('.scroll').evaluate(root => {
    const out = [];
    for (const el of root.querySelectorAll('input, button, select, textarea, [role=combobox], [role=radio]')) {
      const byIds = (a) => (el.getAttribute(a) || '').split(/\s+/).filter(Boolean)
        .map(id => (document.getElementById(id) || {}).innerText || '').join(' ').trim();
      const name = (el.getAttribute('aria-label') || '') || byIds('aria-labelledby') ||
                   (el.labels && el.labels[0] ? el.labels[0].innerText : '') ||
                   (el.innerText || '').trim() || (el.getAttribute('title') || '');
      if (!name.trim()) out.push(el.tagName.toLowerCase() + (el.type ? '[' + el.type + ']' : '') +
                                 '.' + (el.className || '').split(' ')[0]);
    }
    return out;
  });
  expect(unnamed, 'controls with no accessible name').toEqual([]);
});

test('a control is named by its own row label, not just any label', async ({ page }) => {
  // Naming everything is not enough - the name has to be the RIGHT one. Checked against a spread
  // of row types (toggle, slider, colour, keybind, dropdown, applist).
  // (Post-cleanup roster: the smooth-zoom/multi-monitor toggles, sharpness slider, and the
  // colour input left the UI - no color-type row remains, so that widget type is uncovered.)
  const cases = [
    ['Alternate keybinds', 'checkbox'],
    ['Max zoom', 'slider'],
    ['Cursor speed', 'slider'],
    ['Frametime logging', 'checkbox'],
    ['Disable MPO', 'checkbox'],
  ];
  for (const [label, role] of cases)
    await expect(page.getByRole(role, { name: label, exact: false }),
      `${role} named "${label}"`).toHaveCount(1);

  // Value-bearing controls read "<label> <value>" so the binding/selection is spoken with the row.
  await expect(page.getByRole('combobox', { name: /Magnifier engine.*Render/ })).toHaveCount(1);
  await expect(page.getByRole('button', { name: /Zoom in.*Mouse button 5/ })).toHaveCount(1);
  await expect(page.getByRole('button', { name: /Inspect mode.*F2/ })).toHaveCount(1);
});

test('row descriptions are linked to their control, not left orphaned', async ({ page }) => {
  const described = await page.getByRole('slider', { name: /Max zoom/ }).evaluate(el => {
    const id = el.getAttribute('aria-describedby');
    return id ? (document.getElementById(id) || {}).innerText : null;
  });
  expect(described).toContain('How far you can zoom');
});

test('sliders speak their unit instead of a bare number', async ({ page }) => {
  await expect(page.getByRole('slider', { name: /Ease-in duration/ }))
    .toHaveAttribute('aria-valuetext', /seconds/);
  await expect(page.getByRole('slider', { name: /Zoom-in speed/ }))
    .toHaveAttribute('aria-valuetext', /times/);
});

// --- Focus visibility -------------------------------------------------------

test('focus is visible, in both themes', async ({ page }) => {
  // There were zero authored :focus rules before this: the only indicator was Chromium's default
  // outline, computing to rgb(16,16,16) against the dark theme's rgb(14,14,18) background.
  for (const scheme of ['light', 'dark']) {
    await page.emulateMedia({ colorScheme: scheme });
    const ring = await page.evaluate(() => {
      const b = document.querySelector('.ritem');
      b.focus();
      const cs = getComputedStyle(b);
      const rgb = (s) => (s.match(/\d+/g) || []).map(Number).slice(0, 3);
      const lum = (c) => c.map(v => { v /= 255; return v <= .03928 ? v / 12.92 : ((v + .055) / 1.055) ** 2.4; })
        .reduce((a, v, i) => a + v * [.2126, .7152, .0722][i], 0);
      const a = lum(rgb(cs.outlineColor)), z = lum(rgb(getComputedStyle(document.body).backgroundColor));
      return { width: parseFloat(cs.outlineWidth), style: cs.outlineStyle,
               contrast: (Math.max(a, z) + .05) / (Math.min(a, z) + .05) };
    });
    expect(ring.style, scheme).not.toBe('none');
    expect(ring.width, scheme).toBeGreaterThanOrEqual(2);
    // WCAG 1.4.11 non-text contrast.
    expect(ring.contrast, `focus ring contrast in ${scheme}`).toBeGreaterThanOrEqual(3);
  }
  await page.emulateMedia({ colorScheme: null });
});

// --- Dialogs ----------------------------------------------------------------

test('a dialog takes focus, traps Tab, and gives focus back on close', async ({ page }) => {
  const opener = page.getByRole('button', { name: /Manage list/ }).first();   // two applists exist now (lockApps, #221)
  await opener.click();
  const dlg = page.getByRole('dialog');
  await expect(dlg).toBeVisible();

  // Focus must be INSIDE, or a screen reader never announces the dialog at all - which is what
  // was happening to all seven of them.
  expect(await page.evaluate(() =>
    document.querySelector('[role=dialog]').contains(document.activeElement))).toBe(true);

  // Tab cannot escape into the page behind.
  for (let i = 0; i < 12; i++) {
    await page.keyboard.press('Tab');
    expect(await page.evaluate(() =>
      document.querySelector('[role=dialog]').contains(document.activeElement)),
      `escaped the dialog after ${i + 1} tabs`).toBe(true);
  }
  await page.keyboard.press('Shift+Tab');
  expect(await page.evaluate(() =>
    document.querySelector('[role=dialog]').contains(document.activeElement))).toBe(true);

  await page.keyboard.press('Escape');
  await expect(dlg).toHaveCount(0);
  // Focus goes back where it came from, not to the top of the page.
  await expect(opener).toBeFocused();
});

test('Escape works the instant a dialog opens', async ({ page }) => {
  // Regression: focus used to be moved in on a setTimeout, so an Escape pressed immediately after
  // opening landed on the page behind and did nothing.
  await page.getByRole('button', { name: /Manage list/ }).first().click();   // two applists exist now (lockApps, #221)
  await page.keyboard.press('Escape');
  await expect(page.getByRole('dialog')).toHaveCount(0);
});

test('the settings prompts announce themselves too', async ({ page }) => {
  // The unsaved-changes guard - one of the six that never took focus.
  await page.getByRole('slider', { name: /Max zoom/ }).press('ArrowRight');
  await page.getByRole('button', { name: 'Close' }).click();
  await expect(page.getByRole('dialog')).toBeVisible();
  expect(await page.evaluate(() =>
    document.querySelector('[role=dialog]').contains(document.activeElement))).toBe(true);
  // Named by its heading, so the announcement is the question and not "dialog".
  await expect(page.getByRole('dialog')).toHaveAccessibleName('Settings not applied');
});

// --- The dropdown -----------------------------------------------------------

test('the model dropdown is fully keyboard operable', async ({ page }) => {
  const combo = page.getByRole('combobox', { name: /Magnifier engine/ });
  await combo.focus();

  await page.keyboard.press('Enter');                       // open
  await expect(page.getByRole('listbox')).toBeVisible();
  await expect(combo).toHaveAttribute('aria-expanded', 'true');

  // The active option is published, so a screen reader reads each one as you arrow. Arrows did
  // nothing at all before this.
  // The OPEN combobox: the page has four, and only the open one publishes an active descendant.
  const activeName = async () => page.evaluate(() => {
    const c = document.querySelector('[role=combobox][aria-expanded="true"]');
    const id = c && c.getAttribute('aria-activedescendant');
    return id ? document.getElementById(id).innerText : null;
  });
  expect(await activeName()).toBe('Render');
  await page.keyboard.press('ArrowDown');
  expect(await activeName()).toBe('Transform');
  await page.keyboard.press('ArrowUp');
  expect(await activeName()).toBe('Render');
  await page.keyboard.press('Home');
  expect(await activeName()).toBe('Auto');
  await page.keyboard.press('End');
  expect(await activeName()).toBe('Windows Magnifier');

  await page.keyboard.press('Escape');                      // closes without committing
  await expect(page.getByRole('listbox')).toHaveCount(0);
  await expect(combo).toBeFocused();
  await expect(combo).toHaveAccessibleName(/Render/);

  await page.keyboard.press('ArrowDown');                   // reopen and commit
  await page.keyboard.press('ArrowDown');
  await page.keyboard.press('Enter');
  await expect(combo).toHaveAccessibleName(/Transform/);
});

test('the dropdown supports type-ahead like a native select', async ({ page }) => {
  // Retargeted to the model picker: the cursor-visibility select left the UI (2026-08-21).
  const combo = page.getByRole('combobox', { name: /Magnifier engine/ });
  await combo.focus();
  await page.keyboard.press('t');            // "Transform"
  await expect(page.getByRole('listbox')).toBeVisible();
  await page.keyboard.press('Enter');
  await expect(combo).toHaveAccessibleName(/Transform/);
});

// --- Navigation -------------------------------------------------------------

test('the rail is a navigation landmark that marks and moves to the current section', async ({ page }) => {
  await expect(page.getByRole('navigation', { name: 'Settings sections' })).toHaveCount(1);
  await expect(page.getByRole('main')).toHaveCount(1);

  const display = page.getByRole('button', { name: 'Display' });
  await display.focus();
  await page.keyboard.press('Enter');
  await expect(display).toHaveAttribute('aria-current', 'true');
  // Activating a rail item used to scroll the pane and leave focus behind, so nothing was
  // announced. Focus now lands on the section heading.
  await expect(page.getByRole('heading', { name: 'Display' })).toBeFocused();
});

test('the page has a lang, one h1, and hierarchical headings', async ({ page }) => {
  await expect(page.locator('html')).toHaveAttribute('lang', 'en');
  await expect(page.getByRole('heading', { level: 1 })).toHaveCount(1);
  expect(await page.locator('h3, h4, h5, h6').count()).toBe(0);
});

test('decorative icons are hidden from the accessibility tree', async ({ page }) => {
  // Each injected {@html} icon used to surface as a nameless "graphic" inside its labelled button.
  const exposed = await page.evaluate(() =>
    [...document.querySelectorAll('svg')].filter(s =>
      s.getAttribute('aria-hidden') !== 'true' && !s.getAttribute('aria-label')).length);
  expect(exposed).toBe(0);
});

// --- Live announcements -----------------------------------------------------

test('restructuring the page is announced', async ({ page }) => {
  const status = page.locator('[role=status][aria-live=polite]');
  // Toggling advanced adds ~10 rows with no focus change: silent before this.
  const advanced = page.getByRole('checkbox', { name: /Show advanced settings/ });
  await advanced.click();
  await expect(status).toHaveText(/Advanced settings hidden/);
  await advanced.click();
  await expect(status).toHaveText(/Advanced settings shown/);

  // Toggling twice put the value back, so nothing is staged; nudge a slider to arm Apply.
  await page.getByRole('slider', { name: /Max zoom/ }).press('ArrowRight');
  await page.getByRole('button', { name: 'Apply' }).click();
  await expect(status).toHaveText(/Settings applied/);
});

// --- Widgets ----------------------------------------------------------------

// (The segmented-control radio-group test left with the quick-zoom rows - no segmented row
// remains in the schema. Restore it from git history if a segmented row ever returns.)

test('Tab escapes an armed keybind capture instead of binding Tab', async ({ page }) => {
  // Arming used to swallow every key including Tab, so a keyboard user who activated a keycap by
  // mistake had no way out except Escape - which nothing told them about.
  const cap = page.getByRole('button', { name: /Hide cursor/ });
  await cap.focus();
  await page.keyboard.press('Enter');                    // arm
  await expect(cap).toHaveText(/Press a key/);
  await page.keyboard.press('Tab');
  await expect(cap).not.toBeFocused();
  await expect(cap).toHaveText('Unbound');               // Tab was NOT captured
  expect(await page.evaluate(() =>
    window.__sets.filter(s => s.key === 'hideCursorVk' && s.value === '9').length)).toBe(0);
});

test('an armed keycap says so, and carries its instructions as a description', async ({ page }) => {
  const cap = page.getByRole('button', { name: /Hide cursor/ });
  await expect(cap).toHaveAccessibleDescription(/Escape cancels/);
  await cap.click();
  await expect(page.locator('[aria-live=assertive]').filter({ hasText: 'Listening' })).toHaveCount(1);
});

test('the profile menu has valid roles and arrow-key navigation', async ({ page }) => {
  await page.getByRole('button', { name: /Default/ }).click();
  const menu = page.getByRole('menu', { name: 'Profiles' });
  await expect(menu).toBeVisible();
  // The active profile was a tick glyph only - sighted-only state.
  await expect(menu.getByRole('menuitemradio', { name: /Default/ })).toHaveAttribute('aria-checked', 'true');
  await expect(menu.getByRole('menuitemradio', { name: /Gaming/ })).toHaveAttribute('aria-checked', 'false');
  // A menuitem may not contain an interactive child; the "..." is a sibling now.
  expect(await page.evaluate(() =>
    document.querySelectorAll('[role^=menuitem] button').length)).toBe(0);
  // Opening with the keyboard lands focus in the menu, and arrows move within it.
  expect(await page.evaluate(() => document.activeElement.getAttribute('role'))).toMatch(/^menuitem/);
  const before = await page.evaluate(() => document.activeElement.textContent.trim());
  await page.keyboard.press('ArrowDown');
  expect(await page.evaluate(() => document.activeElement.textContent.trim())).not.toBe(before);
});
