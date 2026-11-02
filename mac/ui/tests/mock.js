// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
// The Mac host, mocked for Playwright: bridge.js posts to window.__windMock when there is no
// window.webkit.messageHandlers.wind, and the host answers through window.__windReceive.
//
//   window.__msgs     every message the page posted, in order
//   window.__live     the config the mock host holds (strings, like SettingsWeb.values)
//   window.__mac      the macStatus the mock host reports
//   window.__refuse   keys the host refuses (answers configWriteFailed)
//   window.__hostSend(msg)  push a message to the page, as the host's evaluateJavaScript does
//
// opts.values  config overrides; opts.mac  macStatus overrides; opts.init  inject window.__windInit
// as the real host does (otherwise the first getSession asks with getConfig).
import { expect } from '@playwright/test';

export const MAC_OK = { accessibility: true, screenRecording: true, zoomShortcuts: [], loginItem: false, logLevel: 1 };

// Binds the specs work with. Since 2026-11-01 only Ctrl+scroll, Ctrl+Opt+arrows and Cmd+Esc are
// presets, so the specs give the other actions the binds they press and edit (like scripts/windtest.py).
export const BINDS = { zoomIn: 'mouse4,ctrl+opt+equal', zoomOut: 'mouse3,ctrl+opt+minus', quickZoom: 'ctrl+opt+z', inspect: 'ctrl+opt+i' };

export async function mockHost(page, opts = {}) {
  await page.addInitScript(({ values, mac, init, refuse, resume }) => {
    window.__msgs = [];
    window.__live = { ...values };
    window.__mac = { ...mac };
    window.__refuse = refuse || [];
    window.__hostSend = (msg) => window.__windReceive && window.__windReceive(msg);
    const send = (msg) => queueMicrotask(() => window.__hostSend(msg));
    if (init) window.__windInit = { values: { ...values }, runningModel: values.engine || 'zoom', mac: { ...mac }, resume: !!resume };
    window.__windMock = (msg) => {
      window.__msgs.push(msg);
      if (msg.type === 'getConfig') send({ type: 'config', values: { ...window.__live }, runningModel: window.__live.engine || 'zoom' });
      else if (msg.type === 'setConfig') {
        if (window.__refuse.includes(msg.key)) send({ type: 'configWriteFailed', key: msg.key });
        else window.__live[msg.key] = msg.value;
      } else if (msg.type === 'getMacStatus') send({ type: 'macStatus', ...window.__mac });
    };
  }, { values: { ...BINDS, ...(opts.values || {}) }, mac: { ...MAC_OK, ...(opts.mac || {}) }, init: !!opts.init, refuse: opts.refuse, resume: !!opts.resume });
}

// Messages of one type the page posted.
export const sent = (page, type) => page.evaluate((t) => window.__msgs.filter((m) => m.type === t), type);
// The last value setConfig wrote for a key (undefined when it never wrote it).
export const wrote = async (page, k) => (await sent(page, 'setConfig')).filter((m) => m.key === k).at(-1)?.value;
export const live = (page, k) => page.evaluate((key) => window.__live[key], k);
export const go = (page, g) => page.locator('.side .it[data-g="' + g + '"]').click();
export const row = (page, k) => page.locator('main [data-key="' + k + '"]');
export const css = (loc, prop) => loc.evaluate((el, p) => getComputedStyle(el).getPropertyValue(p), prop);

// Open Settings and wait until the config has loaded (the app is hidden until then).
export async function open(page, path = '/') {
  await page.goto(path);
  await expect(page.locator('.app:not(.pending)')).toBeVisible();
}

export async function advanced(page, on) {
  await go(page, 'prefs');
  const sw = row(page, 'showAdvanced').getByRole('switch');
  if (on) await sw.check({ force: true }); else await sw.uncheck({ force: true });
}

// The Mac page shortcut modifier: Cmd.
export const CMD = 'Meta';
