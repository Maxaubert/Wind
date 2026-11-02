// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
// Hovering any control keeps its text readable (contrast at least 3) on every page and setup step. A plain
// hover rule once outranked .primary and turned the white Get started button's text white.
import { test, expect } from '@playwright/test';
import { mockHost, open, go, MAC_OK } from './mock.js';
const MEASURE = () => {
  const parse = (c) => { const m = c.match(/[\d.]+/g); return m ? m.map(Number) : [0, 0, 0, 0]; };
  const lum = ([r, g, b]) => { const f = (v) => { v /= 255; return v <= .03928 ? v / 12.92 : ((v + .055) / 1.055) ** 2.4; }; return .2126 * f(r) + .7152 * f(g) + .0722 * f(b); };
  window.__contrast = (el) => {
    const cs = getComputedStyle(el); const fg = parse(cs.color);
    let e = el, bg = null;
    while (e) { const b = parse(getComputedStyle(e).backgroundColor); if (b.length < 4 || b[3] > 0.5) { bg = b; break; } e = e.parentElement; }
    bg = bg || [0, 0, 0];
    const a = lum(fg), b = lum(bg); return (Math.max(a, b) + .05) / (Math.min(a, b) + .05);
  };
};
const SEL = 'button, select, a, [role=button], .kc, .it, .th, .chip, .keycap';
async function sweep(page, where, out) {
  const n = await page.locator(SEL).count();
  for (let i = 0; i < n; i++) {
    const el = page.locator(SEL).nth(i);
    if (!(await el.isVisible())) continue;
    const before = await el.evaluate((e) => window.__contrast(e));
    try { await el.hover({ timeout: 800 }); } catch { continue; }
    await page.waitForTimeout(180);
    const after = await el.evaluate((e) => window.__contrast(e));
    const label = await el.evaluate((e) => (e.className + ' ' + (e.textContent || '').trim()).slice(0, 50));
    if (after < 3 && (await el.evaluate((e) => (e.textContent || '').trim().length > 0))) out.push(`${where}: "${label}" ${before.toFixed(1)} -> ${after.toFixed(1)}`);
  }
  await page.mouse.move(1, 1);
}
for (const pal of ['grey']) {
  test('hover keeps text readable (' + pal + ')', async ({ page }) => {
    test.setTimeout(600000);
    const out = [];
    await page.addInitScript(MEASURE);
    await mockHost(page, { init: true, values: { uiPalette: pal, showAdvanced: true }, mac: { ...MAC_OK, screenRecording: false, zoomShortcuts: ['Use keyboard shortcuts to zoom'] } });
    await open(page);
    for (const g of ['hotkeys', 'zoom', 'view', 'prefs', 'about']) { await go(page, g); await page.waitForTimeout(300); await sweep(page, pal + '/' + g, out); }
    await page.goto('/?mode=onboard'); await page.waitForTimeout(500);
    for (let s = 0; s < 6; s++) {
      await sweep(page, pal + '/onboard' + s, out);
      const p = page.locator('.wizfoot .btn.primary');
      if (!(await p.count())) break;
      if (await p.isDisabled()) { const sk = page.getByRole('button', { name: 'Skip this step' }); if (await sk.count()) await sk.click(); else break; }
      else await p.click();
      await page.waitForTimeout(400);
    }
    expect(out).toEqual([]);
  });
}
